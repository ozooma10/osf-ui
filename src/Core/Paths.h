#pragma once

namespace OSFUI::Paths
{
	bool Initialize();

	// Directory containing the plugin DLL, e.g. <game>/Data/SFSE/Plugins
	[[nodiscard]] const std::filesystem::path& PluginDir();

	// Plugin data root, e.g. <game>/Data/SFSE/Plugins/OSFUI
	[[nodiscard]] const std::filesystem::path& DataDir();

	// <data>/views
	[[nodiscard]] std::filesystem::path ViewsDir();

	// Documents/My Games/Starfield, derived from CommonLibSF's log directory; Empty when CommonLibSF cannot resolve the user directory.
	[[nodiscard]] std::filesystem::path StarfieldUserDir();

}
