#pragma once

namespace OSFUI
{
	// Runtime-owned: balance UI::ModifyMenuPauseCounter edges or a leaked increment pauses indefinitely.
	class SimPause
	{
	public:
		// Drive the pause counter from each runtime tick, retrying until RE::UI exists.
		static void Apply(bool a_desired);
	};
}
