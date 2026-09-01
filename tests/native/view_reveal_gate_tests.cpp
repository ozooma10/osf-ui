#include "Views/ViewRevealGate.h"

#include <cassert>
#include <iostream>
#include <optional>

using OSFUI::ViewRevealGate;

namespace
{
	ViewRevealGate::FrameObservation Frame(
		std::uint64_t a_index,
		bool          a_outputSizeKnown = true,
		bool          a_matchesExpectedSize = true,
		std::uint64_t a_generation = 1)
	{
		return {
			.generation = a_generation,
			.index = a_index,
			.outputSizeKnown = a_outputSizeKnown,
			.matchesExpectedSize = a_matchesExpectedSize,
		};
	}
}

int main()
{
	ViewRevealGate gate;

	{
		const auto decision = gate.Observe(Frame(10), 1.0);
		assert(decision.submitFrame);
		assert(!decision.reveal);
		assert(!decision.timedOut);
	}
	assert(!gate.Observe(Frame(10), 1.1).submitFrame);

	gate.Arm();
	assert(gate.Pending());

	// The cached frame from the previous presentation cannot reveal a reopen.
	{
		const auto decision = gate.Observe(Frame(10), 2.0);
		assert(!decision.submitFrame);
		assert(!decision.reveal);
		assert(!decision.timedOut);
	}

	{
		const auto decision = gate.Observe(Frame(11, false), 2.1);
		assert(decision.submitFrame);
		assert(!decision.reveal);
		assert(!decision.timedOut);
	}

	{
		const auto decision = gate.Observe(Frame(11, true, false), 2.2);
		assert(!decision.submitFrame);
		assert(!decision.reveal);
		assert(!decision.timedOut);
	}

	// A fresh, correctly sized frame is submitted before compositor visibility.
	{
		const auto decision = gate.Observe(Frame(12), 2.3);
		assert(decision.submitFrame);
		assert(decision.reveal);
		assert(!decision.timedOut);
		assert(!gate.Pending());
	}

	gate.Arm();
	gate.Cancel();
	assert(!gate.Pending());
	gate.Arm();
	{
		const auto decision = gate.Observe(Frame(12), 3.0);
		assert(!decision.submitFrame);
		assert(!decision.reveal);
	}
	{
		const auto decision = gate.Observe(Frame(12, true, true, 2), 3.1);
		assert(decision.submitFrame);
		assert(decision.reveal);
	}

	ViewRevealGate resizeGate;
	assert(resizeGate.Observe(Frame(100, true, true, 4), 4.0).submitFrame);
	resizeGate.ArmForResize();
	{
		const auto decision = resizeGate.Observe(Frame(101, true, true, 4), 4.1);
		assert(decision.submitFrame);
		assert(!decision.reveal);  // fresh serial, but still the old-size ring
	}
	{
		const auto decision = resizeGate.Observe(Frame(1, true, true, 3), 4.2);
		assert(decision.submitFrame);
		assert(!decision.reveal);  // an older ring can never satisfy a resize
	}
	{
		const auto decision = resizeGate.Observe(Frame(1, true, false, 5), 4.3);
		assert(decision.submitFrame);
		assert(!decision.reveal);  // new ring, intermediate dimensions
	}
	{
		const auto decision = resizeGate.Observe(Frame(1, true, true, 6), 4.4);
		assert(decision.submitFrame);
		assert(decision.reveal);
	}

	// A second mode edge while the first resize is in flight raises the floor.
	resizeGate.Reset();
	assert(resizeGate.Observe(Frame(10, true, true, 10), 5.0).submitFrame);
	resizeGate.ArmForResize();
	assert(!resizeGate.Observe(Frame(1, true, false, 11), 5.1).reveal);
	resizeGate.ArmForResize();
	assert(!resizeGate.Observe(Frame(2, true, true, 11), 5.2).reveal);
	assert(resizeGate.Observe(Frame(1, true, true, 12), 5.3).reveal);

	gate.Reset();
	gate.Arm();
	assert(!gate.Observe(std::nullopt, 10.0).timedOut);
	assert(!gate.Observe(std::nullopt, 20.0).timedOut);  // charged only 0.25 s
	ViewRevealGate::Decision timeout;
	for (int i = 1; i <= 10; ++i) {
		timeout = gate.Observe(std::nullopt, 20.0 + i * 0.25);
		assert(!timeout.timedOut);
	}
	timeout = gate.Observe(std::nullopt, 23.0);
	assert(timeout.timedOut);
	assert(timeout.heldSeconds == ViewRevealGate::kTimeoutSeconds);
	assert(!gate.Pending());
	assert(!gate.Observe(std::nullopt, 24.0).timedOut);

	gate.Arm();
	assert(!gate.Observe(std::nullopt, 30.0).timedOut);
	const auto afterStall = gate.Observe(Frame(1), 300.0);
	assert(afterStall.submitFrame);
	assert(afterStall.reveal);
	assert(!afterStall.timedOut);

	std::cout << "view_reveal_gate_tests: ok\n";
	return 0;
}
