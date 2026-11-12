#pragma once

namespace OSFUI
{
	// Runtime owns one input layer to block player controls on all devices.
	class ControlLayer
	{
	public:
		// Call every runtime tick; allocates the layer on first use.
		static void Apply(bool a_engage);
		static bool InstallSaveSafety();
		static void SaveWindow(bool a_saving);
		static void BeginRequestedSave();
		static void EndRequestedSave();
		static void BeginLoad();
		static void EndLoad();

	private:
		ControlLayer() = default;
	};
}
