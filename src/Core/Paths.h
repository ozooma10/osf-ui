#pragma once

namespace OSFUI::Paths
{
	bool Initialize();

	// Plugin data root, e.g. <game>/Data/SFSE/Plugins/OSF/UI
	[[nodiscard]] const std::filesystem::path& DataDir();

	// <data>/views
	[[nodiscard]] std::filesystem::path ViewsDir();
}
