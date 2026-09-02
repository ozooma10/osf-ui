#pragma once

namespace OSFUI::Paths
{
	bool Initialize();

	// Directory containing the plugin DLL, e.g. <game>/Data/SFSE/Plugins
	[[nodiscard]] const std::filesystem::path& PluginDir();

	// Plugin data root, e.g. <game>/Data/SFSE/Plugins/OSF/UI
	[[nodiscard]] const std::filesystem::path& DataDir();

	// <data>/views
	[[nodiscard]] std::filesystem::path ViewsDir();

	// Documents/My Games/Starfield, or empty when CommonLibSF cannot resolve it.
	[[nodiscard]] std::filesystem::path StarfieldUserDir();

}
