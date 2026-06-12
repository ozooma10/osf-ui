#pragma once

namespace SWUI::Paths
{
	// Resolves all paths relative to the loaded plugin DLL. Must be called once
	// from SFSE_PLUGIN_LOAD before anything reads config or views.
	// Returns false if the module path cannot be determined (paths stay empty).
	bool Initialize();

	// Directory containing the plugin DLL, e.g. <game>/Data/SFSE/Plugins
	[[nodiscard]] const std::filesystem::path& PluginDir();

	// Plugin data root, e.g. <game>/Data/SFSE/Plugins/StarfieldWebUI
	[[nodiscard]] const std::filesystem::path& DataDir();

	// <data>/config.json
	[[nodiscard]] std::filesystem::path ConfigFile();

	// <data>/views
	[[nodiscard]] std::filesystem::path ViewsDir();
}
