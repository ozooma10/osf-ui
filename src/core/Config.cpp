#include "core/Config.h"

#include "runtime/Json.h"

#include <chrono>

namespace OSFUI
{
	namespace
	{
		// Every key the parser reads. config.json is host-owned, so an unknown
		// key is a typo, never version skew. Keep in lockstep with the reads below.
		constexpr std::initializer_list<std::string_view> kKnownKeys = {
			"configVersion", "enabled", "renderer", "compositor",
			"inputSource", "captureInput", "hardwareCursor", "focusMenu",
			"engineInput", "pauseMenuEntry", "pauseMenuEntryLabel", "pauseMenuEntryView",
			"view", "views", "devMode", "devReloadKey", "bugReportEndpoint",
#if defined(OSFUI_WITH_WORLD_SURFACES)
			"worldSurfaceView",
			"worldSurfaceWidth", "worldSurfaceHeight",
			"worldSurfaceTargetWidth", "worldSurfaceTargetHeight",
#endif
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
		config.renderer = Json::GetString(*json, "renderer", config.renderer);
		config.compositor = Json::GetString(*json, "compositor", config.compositor);
		config.inputSource = Json::GetString(*json, "inputSource", config.inputSource);
		config.captureInput = Json::GetBool(*json, "captureInput", config.captureInput);
		config.hardwareCursor = Json::GetBool(*json, "hardwareCursor", config.hardwareCursor);
		config.focusMenu = Json::GetBool(*json, "focusMenu", config.focusMenu);
		config.engineInput = Json::GetBool(*json, "engineInput", config.engineInput);
		config.pauseMenuEntry = Json::GetBool(*json, "pauseMenuEntry", config.pauseMenuEntry);
		config.pauseMenuEntryLabel = Json::GetString(*json, "pauseMenuEntryLabel", config.pauseMenuEntryLabel);
		config.pauseMenuEntryView = Json::GetString(*json, "pauseMenuEntryView", config.pauseMenuEntryView);
		config.view = Json::GetString(*json, "view", config.view);
		config.views = Json::GetStringArray(*json, "views");
#if defined(OSFUI_WITH_WORLD_SURFACES)
		config.worldSurfaceView = Json::GetString(*json, "worldSurfaceView", config.worldSurfaceView);
		const auto boundedUInt = [&json](std::string_view a_key, std::uint32_t a_default,
			std::uint32_t a_min, std::uint32_t a_max) {
			const auto value = Json::GetInt(*json, a_key, a_default);
			return static_cast<std::uint32_t>((std::clamp)(value,
				static_cast<std::int64_t>(a_min), static_cast<std::int64_t>(a_max)));
		};
		config.worldSurfaceWidth = boundedUInt("worldSurfaceWidth", config.worldSurfaceWidth, 64, 4096);
		config.worldSurfaceHeight = boundedUInt("worldSurfaceHeight", config.worldSurfaceHeight, 64, 4096);
		config.worldSurfaceTargetWidth = boundedUInt("worldSurfaceTargetWidth", config.worldSurfaceTargetWidth, 1, 16384);
		config.worldSurfaceTargetHeight = boundedUInt("worldSurfaceTargetHeight", config.worldSurfaceTargetHeight, 1, 16384);
#endif
		config.devMode = Json::GetBool(*json, "devMode", config.devMode);
		ApplyAuthorModeMarker(config, a_path);
		config.devReloadKey = Json::GetString(*json, "devReloadKey", config.devReloadKey);
		config.bugReportEndpoint = Json::GetString(*json, "bugReportEndpoint", config.bugReportEndpoint);
		if (!config.bugReportEndpoint.empty() &&
			(!config.bugReportEndpoint.starts_with("https://") ||
			 config.bugReportEndpoint.size() > 512 ||
			 config.bugReportEndpoint.find_first_of(" \t\r\n\"") != std::string::npos)) {
			REX::WARN("Config: bugReportEndpoint must be a bounded HTTPS URL without whitespace; automatic reports disabled");
			config.bugReportEndpoint.clear();
		}

		REX::INFO("Config: loaded {} (renderer={}, compositor={}, inputSource={}, captureInput={}, hardwareCursor={}, focusMenu={}, view={}, devMode={})",
			a_path.string(), config.renderer, config.compositor, config.inputSource, config.captureInput, config.hardwareCursor, config.focusMenu, config.view, config.devMode);
		return config;
	}
}
