#include "core/Config.h"

#include "runtime/Json.h"

namespace OSFUI
{
	namespace
	{
		// User-facing knobs that moved into the `osfui` settings schema
		// (api-freeze-plan item 7). The lenient parser would silently ignore
		// them; call out the move instead so a user editing config.json learns
		// where the knob went. One-release courtesy — drop after 1.x settles.
		constexpr std::string_view kMovedToSettings[] = {
			"toggleKey", "consoleKey", "disableControls", "pauseMenuEntry", "vanillaKeyConflicts"
		};

		// Every key the parser reads (the item-8 typo diagnostic — config.json
		// is host-owned, so an unknown key can only be a typo, never version
		// skew). Keep in lockstep with the reads below.
		constexpr std::initializer_list<std::string_view> kKnownKeys = {
			"configVersion", "enabled", "startVisible", "renderer", "compositor",
			"inputSource", "captureInput", "hardwareCursor", "focusMenu",
			"engineInput", "pauseMenuEntryLabel", "pauseMenuEntryView",
			"view", "views", "allowNetwork", "devMode", "devReloadKey",
			// moved keys are "known" (they get the dedicated INFO, not the typo WARN)
			"toggleKey", "consoleKey", "disableControls", "pauseMenuEntry", "vanillaKeyConflicts",
			// dropped: the Tab focus-cycle died with the single-menu stack
			"focusKey",
		};
	}

	Config Config::Load(const std::filesystem::path& a_path)
	{
		Config config;

		std::error_code ec;
		if (!std::filesystem::exists(a_path, ec)) {
			REX::WARN("Config: {} not found; using built-in defaults", a_path.string());
			return config;
		}

		const auto json = Json::ParseFile(a_path);
		if (!json || !json->is_object()) {
			REX::ERROR("Config: {} is not a valid JSON object; using built-in defaults", a_path.string());
			return config;
		}

		// Format stamp + migration hook (item 8): newer file = parse leniently
		// (unknown fields ignore themselves); older = where migrations would
		// run (none exist yet — the hook is the point).
		if (const auto v = Json::GetInt(*json, "configVersion", kConfigVersion); v > kConfigVersion) {
			REX::INFO("Config: {} declares configVersion {} (this build knows {}) — written by a newer OSF UI; unknown fields are ignored",
				a_path.string(), v, kConfigVersion);
		}
		Json::ReportUnknownKeys(*json, kKnownKeys, "Config: " + a_path.string(), /*a_warn=*/true);
		for (const auto key : kMovedToSettings) {
			if (json->contains(key)) {
				REX::INFO("Config: '{}' is now managed in Mod Settings (F10 -> OSF UI) and persists under Documents; the config.json value is ignored", key);
			}
		}
		if (json->contains("focusKey")) {
			REX::INFO("Config: 'focusKey' was removed (the Tab focus-cycle retired with the single-menu stack); the value is ignored");
		}

		config.enabled = Json::GetBool(*json, "enabled", config.enabled);
		config.startVisible = Json::GetBool(*json, "startVisible", config.startVisible);
		config.renderer = Json::GetString(*json, "renderer", config.renderer);
		config.compositor = Json::GetString(*json, "compositor", config.compositor);
		config.inputSource = Json::GetString(*json, "inputSource", config.inputSource);
		config.captureInput = Json::GetBool(*json, "captureInput", config.captureInput);
		config.hardwareCursor = Json::GetBool(*json, "hardwareCursor", config.hardwareCursor);
		config.focusMenu = Json::GetBool(*json, "focusMenu", config.focusMenu);
		config.engineInput = Json::GetBool(*json, "engineInput", config.engineInput);
		config.pauseMenuEntryLabel = Json::GetString(*json, "pauseMenuEntryLabel", config.pauseMenuEntryLabel);
		config.pauseMenuEntryView = Json::GetString(*json, "pauseMenuEntryView", config.pauseMenuEntryView);
		config.view = Json::GetString(*json, "view", config.view);
		if (const auto it = json->find("views"); it != json->end() && it->is_array()) {
			for (const auto& v : *it) {
				if (v.is_string()) {
					config.views.push_back(v.get<std::string>());
				}
			}
		}
		config.allowNetwork = Json::GetBool(*json, "allowNetwork", config.allowNetwork);
		config.devMode = Json::GetBool(*json, "devMode", config.devMode);
		config.devReloadKey = Json::GetString(*json, "devReloadKey", config.devReloadKey);

		if (config.allowNetwork) {
			// Nothing in this codebase performs network access; refuse the flag
			// loudly so nobody assumes it works.
			REX::WARN("Config: allowNetwork=true is not supported in this build; forcing false");
			config.allowNetwork = false;
		}

		REX::INFO("Config: loaded {} (renderer={}, compositor={}, inputSource={}, captureInput={}, hardwareCursor={}, focusMenu={}, view={}, devMode={})",
			a_path.string(), config.renderer, config.compositor, config.inputSource, config.captureInput, config.hardwareCursor, config.focusMenu, config.view, config.devMode);
		return config;
	}
}
