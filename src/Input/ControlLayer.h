#pragma once

namespace OSFUI
{
	// Main-thread only: retain one session layer and toggle its mask to gate every input device.
	class ControlLayer
	{
	public:
		// Drive the retained layer from each main-thread tick, allocating it on first use.
		static void Apply(bool a_engage);

	private:
		ControlLayer() = default;
	};
}
