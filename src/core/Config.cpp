#include "core/Config.h"

#include "core/StringUtil.h"
#include "runtime/Json.h"

#include <charconv>
#include <chrono>

namespace OSFUI
{
	namespace
	{

		// Every key the parser reads. config.json is host-owned, so an unknown
		// key is a typo, never version skew. Keep in lockstep with the reads below.
		constexpr std::initializer_list<std::string_view> kKnownKeys = {
			"configVersion", "enabled",
			"inputSource", "captureInput", "hardwareCursor", "focusMenu",
			"engineInput", "pauseMenuEntryLabel", "pauseMenuEntryView",
			// 'views'/'warmViews' are recognized so a v1 file does not read as a
			// typo, but they are deprecated no-ops (see the parse site).
			"view", "views", "warmViews", "devMode",
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
				!Json::GetBool(*marker, "enabled", false)) {
				return;
			}
			const auto expiresAt = Json::GetInt(*marker, "expiresAt", 0);
			const auto now = std::chrono::duration_cast<std::chrono::seconds>(
				std::chrono::system_clock::now().time_since_epoch()).count();
			if (expiresAt <= now) {
				REX::WARN("Config: ignored expired author-mode marker {}", markerPath.string());
				return;
			}
			a_config.devMode = true;
			REX::INFO("Config: temporary author mode enabled by {} (expires in {} seconds)",
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

		// Format stamp + migration hook. Newer file: parse leniently, ignoring
		// unknown fields. Older file: where migrations would run (none yet).
		Json::CheckFormatVersion(*json, "configVersion", kConfigVersion, "Config: " + a_path.string());
		Json::ReportUnknownKeys(*json, kKnownKeys, "Config: " + a_path.string(), /*a_warn=*/true);

		config.enabled = Json::GetBool(*json, "enabled", config.enabled);
		config.inputSource = Json::GetString(*json, "inputSource", config.inputSource);
		config.captureInput = Json::GetBool(*json, "captureInput", config.captureInput);
		config.hardwareCursor = Json::GetBool(*json, "hardwareCursor", config.hardwareCursor);
		config.focusMenu = Json::GetBool(*json, "focusMenu", config.focusMenu);
		config.engineInput = Json::GetBool(*json, "engineInput", config.engineInput);
		config.pauseMenuEntryLabel = Json::GetString(*json, "pauseMenuEntryLabel", config.pauseMenuEntryLabel);
		config.pauseMenuEntryView = Json::GetString(*json, "pauseMenuEntryView", config.pauseMenuEntryView);
		config.view = Json::GetString(*json, "view", config.view);
		// configVersion 2 removed the central view lists. HUD auto-start is the
		// player's choice in Mod Settings (manifest openOnStart is the author
		// default); every other view loads on first open.
		for (const auto* legacy : { "views", "warmViews" }) {
			if (json->contains(legacy)) {
				REX::WARN("Config: '{}' is deprecated and ignored (configVersion 2) — "
						  "HUD auto-start is set per HUD in Mod Settings; other views load on first open",
					legacy);
			}
		}
		config.devMode = Json::GetBool(*json, "devMode", config.devMode);
		ApplyAuthorModeMarker(config, a_path);

		REX::INFO("Config: loaded {} (inputSource={}, captureInput={}, hardwareCursor={}, focusMenu={}, view={}, devMode={})",
			a_path.string(), config.inputSource, config.captureInput, config.hardwareCursor, config.focusMenu, config.view, config.devMode);
		return config;
	}
}
