#include "core/Config.h"

#include "core/StringUtil.h"
#include "runtime/Json.h"

#include <charconv>
#include <chrono>

namespace OSFUI
{
	namespace
	{

		// Every key the parser reads. config.json is OSF UI runtime-owned, so an unknown
		// key is a typo, never version skew. Keep in lockstep with the reads below.
		constexpr std::initializer_list<std::string_view> kKnownKeys = {
			"configVersion", "enabled",
			"pauseMenuEntryLabel", "pauseMenuEntryView",
			"view", "devMode",
		};


		void ApplyAuthorModeMarker(Config& a_config, const std::filesystem::path& a_configPath)
		{
			const auto markerPath = a_configPath.parent_path() / ".author-mode.json";
			std::error_code ec;
			if (!std::filesystem::exists(markerPath, ec)) {
				return;
			}
			const auto marker = Json::ParseFile(markerPath);
			if (!marker || !marker->is_object() ||
				!Json::Get(*marker, "enabled", false)) {
				return;
			}
			const auto expiresAt = Json::Get(*marker, "expiresAt", 0);
			const auto now = std::chrono::duration_cast<std::chrono::seconds>(
				std::chrono::system_clock::now().time_since_epoch()).count();
			if (expiresAt <= now) {
				REX::WARN("Config: ignored expired author-mode marker {}", markerPath.string());
				return;
			}
			a_config.devMode = true;
			REX::INFO("Config: temporary developer mode enabled by author-mode marker {} (expires in {} seconds)",
				markerPath.string(), expiresAt - now);
		}
	}


	Config Config::Load(const std::filesystem::path& a_path)
	{
		Config config;

		std::error_code ec;
		if (!std::filesystem::exists(a_path, ec)) {
			REX::WARN("Config: {} not found; using built-in defaults", a_path.string());
			ApplyAuthorModeMarker(config, a_path);
			return config;
		}

		const auto json = Json::ParseFile(a_path);
		if (!json || !json->is_object()) {
			REX::ERROR("Config: {} is not a valid JSON object; using built-in defaults", a_path.string());
			ApplyAuthorModeMarker(config, a_path);
			return config;
		}

		// Newer and older files parse leniently. There is no config-v1
		// compatibility layer: removed fields are ordinary unknown keys.
		Json::CheckFormatVersion(*json, "configVersion", kConfigVersion, "Config: " + a_path.string());
		Json::ReportUnknownKeys(*json, kKnownKeys, "Config: " + a_path.string(), /*a_warn=*/true);

		config.enabled = Json::Get(*json, "enabled", config.enabled);
		config.pauseMenuEntryLabel = Json::Get(*json, "pauseMenuEntryLabel", config.pauseMenuEntryLabel);
		config.pauseMenuEntryView = Json::Get(*json, "pauseMenuEntryView", config.pauseMenuEntryView);
		config.view = Json::Get(*json, "view", config.view);
		config.devMode = Json::Get(*json, "devMode", config.devMode);
		ApplyAuthorModeMarker(config, a_path);

		REX::INFO("Config: loaded {} (enabled={}, view={}, devMode={})",
			a_path.string(), config.enabled, config.view, config.devMode);
		return config;
	}
}
