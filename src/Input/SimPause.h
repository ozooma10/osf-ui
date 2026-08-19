#pragma once

namespace OSFUI
{
	// Main-thread only: balance UI::ModifyMenuPauseCounter edges or a leaked increment pauses indefinitely.
	class SimPause
	{
	public:
		// Drive the pause counter from each main-thread tick, retrying until RE::UI exists.
		static void Apply(bool a_desired);
	};
}
