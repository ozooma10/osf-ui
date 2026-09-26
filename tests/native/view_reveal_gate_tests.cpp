#include "Views/ViewRevealGate.h"

#include <cassert>
#include <iostream>

using OSFUI::ViewRevealGate;

int main()
{
	ViewRevealGate gate;

	// Unarmed: frames are ignored.
	{
		const auto decision = gate.Observe(true, 1.0);
		assert(!decision.reveal);
		assert(!decision.timedOut);
	}

	gate.Arm();
	assert(gate.Pending());
	assert(!gate.Observe(false, 2.0).reveal);
	{
		const auto decision = gate.Observe(true, 2.1);
		assert(decision.reveal);
		assert(!decision.timedOut);
		assert(!gate.Pending());
	}

	gate.Arm();
	gate.Cancel();
	assert(!gate.Pending());
	assert(!gate.Observe(true, 3.0).reveal);

	gate.Arm();
	assert(!gate.Observe(false, 10.0).timedOut);
	assert(!gate.Observe(false, 20.0).timedOut);  // charged only 0.25 s
	ViewRevealGate::Decision timeout;
	for (int i = 1; i <= 10; ++i) {
		timeout = gate.Observe(false, 20.0 + i * 0.25);
		assert(!timeout.timedOut);
	}
	timeout = gate.Observe(false, 23.0);
	assert(timeout.timedOut);
	assert(timeout.heldSeconds == ViewRevealGate::kTimeoutSeconds);
	assert(!gate.Pending());
	assert(!gate.Observe(false, 24.0).timedOut);

	// A frame that is ready on the first tick after a stall wins over the timeout.
	gate.Arm();
	assert(!gate.Observe(false, 30.0).timedOut);
	const auto afterStall = gate.Observe(true, 300.0);
	assert(afterStall.reveal);
	assert(!afterStall.timedOut);

	std::cout << "view_reveal_gate_tests: ok\n";
	return 0;
}
