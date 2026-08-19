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
		bool          a_matchesExpectedSize = true)
	{
		return {
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
