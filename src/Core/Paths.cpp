#include "Core/Paths.h"

#include "Core/Version.h"

#include "REX/FModule.h"
#include "SFSE/Logger.h"

namespace OSFUI::Paths
{
	namespace
	{
		std::filesystem::path g_pluginDir;
		std::filesystem::path g_dataDir;
	}

	bool Initialize()
	{
		const std::filesystem::path gamePath = REX::FModule::GetExecutingModule().GetFileName();
		g_pluginDir = gamePath.parent_path() / "Data" / "SFSE" / "Plugins";
		g_dataDir = g_pluginDir / kDataFolderName;

		REX::INFO("Paths: plugin dir = {}", g_pluginDir.string());
		REX::INFO("Paths: data dir   = {}", g_dataDir.string());

		std::error_code ec;
		if (!std::filesystem::exists(g_dataDir, ec)) {
			REX::WARN("Paths: OSF UI data directory does not exist ({}); no packaged views are available", g_dataDir.string());
		}
		return true;
	}

	const std::filesystem::path& PluginDir()
	{
		return g_pluginDir;
	}

	const std::filesystem::path& DataDir()
	{
		return g_dataDir;
	}

	std::filesystem::path ViewsDir()
	{
		return g_dataDir / "views";
	}

	std::filesystem::path StarfieldUserDir()
	{
		const auto logDir = SFSE::log::log_directory();
		return logDir ? logDir->parent_path().parent_path() : std::filesystem::path{};
	}

}
