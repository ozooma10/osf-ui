#pragma once

namespace OSFUI
{
	// Runtime-owned: retain one session layer and toggle its mask to gate every input device.
	class ControlLayer
	{
	public:
		// Drive the retained layer from each runtime tick, allocating it on first use.
		static void Apply(bool a_engage);

	private:
		ControlLayer() = default;
	};
}
