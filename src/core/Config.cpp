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
			"view", "views", "devMode",
#if defined(OSFUI_WITH_WORLD_SURFACES)
			"worldSurfaces",
#endif
		};

#if defined(OSFUI_WITH_WORLD_SURFACES)
		// Lenient like every other key: invalid entries are dropped with a WARN
		// naming the reason; the rest of the config (and array) still applies.
		void ParseWorldSurfaces(const Json::Value& a_json, Config& a_config)
		{
			const auto it = a_json.find("worldSurfaces");
			if (it == a_json.end()) {
				return;
			}
			if (!it->is_array()) {
				REX::WARN("Config: worldSurfaces is not an array; ignored");
				return;
			}
			const auto boundedUInt = [](const Json::Value& a_obj, std::string_view a_key,
				std::uint32_t a_default, std::uint32_t a_min, std::uint32_t a_max) {
				const auto value = Json::GetInt(a_obj, a_key, a_default);
				return static_cast<std::uint32_t>((std::clamp)(value,
					static_cast<std::int64_t>(a_min), static_cast<std::int64_t>(a_max)));
			};
			std::size_t index = 0;
			for (const auto& element : *it) {
				const auto entryIndex = index++;
				if (!element.is_object()) {
					REX::WARN("Config: worldSurfaces[{}] is not an object; dropped", entryIndex);
					continue;
				}
				Config::WorldSurfaceEntry entry;
				entry.view = Json::GetString(element, "view", "");
				if (entry.view.empty()) {
					REX::WARN("Config: worldSurfaces[{}] has no view id; dropped", entryIndex);
					continue;
				}
				entry.width = boundedUInt(element, "width", entry.width, 64, 4096);
				entry.height = boundedUInt(element, "height", entry.height, 64, 4096);
				entry.placeholderWidth = static_cast<std::uint32_t>(
					(std::max)(Json::GetInt(element, "placeholderWidth", 0), std::int64_t{ 0 }));
				entry.placeholderHeight = static_cast<std::uint32_t>(
					(std::max)(Json::GetInt(element, "placeholderHeight", 0), std::int64_t{ 0 }));
				if (const auto reason = Config::CheckPlaceholderSize(
						entry.placeholderWidth, entry.placeholderHeight);
					!reason.empty()) {
					REX::WARN("Config: worldSurfaces[{}] ('{}') placeholder {}x{} rejected: {}; dropped",
						entryIndex, entry.view, entry.placeholderWidth, entry.placeholderHeight, reason);
					continue;
				}
				const auto duplicate = std::ranges::find_if(a_config.worldSurfaces,
					[&entry](const Config::WorldSurfaceEntry& a_kept) {
						return (a_kept.placeholderWidth == entry.placeholderWidth &&
								   a_kept.placeholderHeight == entry.placeholderHeight) ||
					           a_kept.view == entry.view;
					});
				if (duplicate != a_config.worldSurfaces.end()) {
					// The SRV hook disambiguates surfaces by placeholder size alone,
					// and bridge routing disambiguates by view id — neither may repeat.
					REX::WARN("Config: worldSurfaces[{}] ('{}', placeholder {}x{}) duplicates an "
							  "earlier entry's view id or placeholder size; dropped",
						entryIndex, entry.view, entry.placeholderWidth, entry.placeholderHeight);
					continue;
				}
				if (a_config.worldSurfaces.size() >= Config::kMaxWorldSurfaces) {
					REX::WARN("Config: worldSurfaces[{}] ('{}') exceeds the {}-surface cap; dropped",
						entryIndex, entry.view, Config::kMaxWorldSurfaces);
					continue;
				}
				a_config.worldSurfaces.push_back(std::move(entry));
			}
			if (a_config.worldSurfaces.size() > 2) {
				REX::WARN("Config: {} world surfaces configured — each runs its own browser "
						  "host process; expect the memory cost of {} extra WebView2 instances",
					a_config.worldSurfaces.size(), a_config.worldSurfaces.size());
			}
		}
#endif

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

#if defined(OSFUI_WITH_WORLD_SURFACES)
	std::string_view Config::CheckPlaceholderSize(std::uint32_t a_width, std::uint32_t a_height)
	{
		if (a_width != a_height) {
			// Squareness excludes every 16:9/16:10 backbuffer and post-buffer
			// shape in one rule.
			return "not square";
		}
		if (a_width < 256 || a_width > 8192) {
			return "outside the 256-8192 range";
		}
		if ((a_width & (a_width - 1)) == 0) {
			// Powers of two are the shape of atlases and mip-chained textures.
			return "a power of two";
		}
		return {};
	}
#endif

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
		ParseWorldSurfaces(*json, config);
#endif
		config.devMode = Json::GetBool(*json, "devMode", config.devMode);
		ApplyAuthorModeMarker(config, a_path);

		REX::INFO("Config: loaded {} (renderer={}, compositor={}, inputSource={}, captureInput={}, hardwareCursor={}, focusMenu={}, view={}, devMode={})",
			a_path.string(), config.renderer, config.compositor, config.inputSource, config.captureInput, config.hardwareCursor, config.focusMenu, config.view, config.devMode);
		return config;
	}
}
