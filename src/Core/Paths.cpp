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
		const std::filesystem::path modulePath = REX::FModule::GetCurrentModule().GetFileName();
		g_pluginDir = modulePath.parent_path();
		g_dataDir = g_pluginDir / kDataFolderName;

		REX::INFO("Paths: plugin dir = {}", g_pluginDir.string());
		REX::INFO("Paths: data dir   = {}", g_dataDir.string());

		std::error_code ec;
		if (!std::filesystem::exists(g_dataDir, ec)) {
			REX::WARN("Paths: data dir does not exist ({}); settings and views will use built-in defaults", g_dataDir.string());
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
