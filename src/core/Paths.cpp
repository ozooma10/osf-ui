#include "core/Paths.h"

#include "core/Version.h"
#include "platform/WindowsPlatform.h"

namespace PrismaSF::Paths
{
	namespace
	{
		std::filesystem::path g_pluginDir;
		std::filesystem::path g_dataDir;
	}

	bool Initialize()
	{
		const auto modulePath = Platform::GetThisModulePath();
		if (modulePath.empty()) {
			REX::ERROR("Paths: failed to resolve plugin module path");
			return false;
		}

		g_pluginDir = modulePath.parent_path();
		g_dataDir = g_pluginDir / kDataFolderName;

		REX::INFO("Paths: plugin dir = {}", g_pluginDir.string());
		REX::INFO("Paths: data dir   = {}", g_dataDir.string());

		std::error_code ec;
		if (!std::filesystem::exists(g_dataDir, ec)) {
			REX::WARN("Paths: data dir does not exist ({}); config and views will use built-in defaults", g_dataDir.string());
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

	std::filesystem::path ConfigFile()
	{
		return g_dataDir / "config.json";
	}

	std::filesystem::path ViewsDir()
	{
		return g_dataDir / "views";
	}
}
