#include "Input/GamepadNavigation.h"

#include <cassert>
#include <iostream>

int main()
{
	using OSFUI::GamepadNavigation;

	GamepadNavigation nav;

	// A normal upward flick is exactly one step, including small release jitter.
	assert(nav.Update(0.0f, 0.60f, 0.00) == GamepadNavigation::kUp);
	assert(nav.Update(0.0f, 0.60f, 0.20) == 0);
	assert(nav.Update(0.0f, 0.40f, 0.40) == 0);
	assert(nav.Update(0.0f, 0.60f, 0.50) == 0);

	// A deliberate hold repeats only after the longer initial delay.
	assert(nav.Update(0.0f, 0.60f, 0.55) == GamepadNavigation::kUp);
	assert(nav.Update(0.0f, 0.60f, 0.60) == 0);
	assert(nav.Update(0.0f, 0.60f, 0.68) == GamepadNavigation::kUp);

	// Recenter arms a new discrete movement.
	assert(nav.Update(0.0f, 0.30f, 0.70) == 0);
	assert(nav.Update(0.0f, 0.60f, 0.71) == GamepadNavigation::kUp);

	// Below the engage threshold is not navigation.
	nav.Reset();
	assert(nav.Update(0.0f, 0.50f, 1.00) == 0);

	// A diagonal produces only its dominant direction, never two key taps.
	assert(nav.Update(0.60f, 0.80f, 1.10) == GamepadNavigation::kUp);
	nav.Reset();
	assert(nav.Update(-0.90f, 0.70f, 1.20) == GamepadNavigation::kLeft);

	// Rolling directly to another axis cleanly changes direction once.
	assert(nav.Update(0.80f, 0.10f, 1.30) == GamepadNavigation::kRight);
	assert(nav.Update(0.80f, 0.10f, 1.40) == 0);

	std::cout << "gamepad navigation tests passed\n";
}
