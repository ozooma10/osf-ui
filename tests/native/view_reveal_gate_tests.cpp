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

	// While no reveal is pending, every renderer frame flows straight through
	// and also becomes the freshness baseline for the next presentation.
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

	// A fresh frame is submitted while hidden, but an unknown output size keeps
	// the reveal held until the D3D12 seam reports its dimensions.
	{
		const auto decision = gate.Observe(Frame(11, false), 2.1);
		assert(decision.submitFrame);
		assert(!decision.reveal);
		assert(!decision.timedOut);
	}

	// Knowing the output size is insufficient while the browser texture still
	// has its old dimensions.
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

	// Closing during a held reopen cancels it without discarding the last frame
	// baseline; reopening still rejects that cached frame.
	gate.Arm();
	gate.Cancel();
	assert(!gate.Pending());
	gate.Arm();
	{
		const auto decision = gate.Observe(Frame(12), 3.0);
		assert(!decision.submitFrame);
		assert(!decision.reveal);
	}

	// Held time advances only while polled and caps a long game stall to one
	// small step. The timeout is one-shot and clears the gate fail-closed.
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

	// Readiness wins before timeout charging: a presentable frame on the first
	// tick after a long stall must reveal, not close the menu.
	gate.Arm();
	assert(!gate.Observe(std::nullopt, 30.0).timedOut);
	const auto afterStall = gate.Observe(Frame(1), 300.0);
	assert(afterStall.submitFrame);
	assert(afterStall.reveal);
	assert(!afterStall.timedOut);

	std::cout << "view_reveal_gate_tests: ok\n";
	return 0;
}
