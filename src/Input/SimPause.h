#pragma once

namespace OSFUI
{
	// Runtime owns one pause-counter increment; release it to unpause.
	class SimPause
	{
	public:
		// Call every runtime tick; retries until RE::UI exists.
		static void Apply(bool a_desired);
	};
}
