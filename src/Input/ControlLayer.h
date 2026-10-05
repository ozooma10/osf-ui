#pragma once

namespace OSFUI
{
	// Runtime owns one input layer to block player controls on all devices.
	class ControlLayer
	{
	public:
		// Call every runtime tick; allocates the layer on first use.
		static void Apply(bool a_engage);

	private:
		ControlLayer() = default;
	};
}
