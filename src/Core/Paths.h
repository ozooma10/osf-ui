#pragma once

namespace OSFUI::Paths
{
	void Initialize();

	// Plugin data root, e.g. <game>/Data/SFSE/Plugins/OSF/UI
	[[nodiscard]] const std::filesystem::path& DataDir();

	[[nodiscard]] std::filesystem::path ViewsDir();
}
