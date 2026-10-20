#pragma once

#include <cstdint>
#include <nlohmann/json_fwd.hpp>

namespace OSFUI
{
	// Default size in CSS pixels.
	inline constexpr std::uint32_t kDefaultViewWidth{ 1600 };
	inline constexpr std::uint32_t kDefaultViewHeight{ 900 };

	// Menus may own input/pause; HUDs render over gameplay without capture.
	enum class ViewKind : std::uint8_t
	{
		Menu,
		Hud,
		Surface,
	};

	// Mirrors OSF/UI/views/<modId>/<viewName>/manifest.json.
	struct ViewManifest
	{
		// <modId>/<viewName>, from the folder path.
		std::string           id;
		std::string           title;
		std::string           description;
		std::string           launcherMod; // Nonempty opts this menu into the shared launcher.
		std::string           launcherModTitle;
		// Mod ID comes from its folder under views/.
		std::string           mod;
		std::string           entry{ "index.html" };
		// CSS pixels, scaled to the output height.
		std::uint32_t         width{ kDefaultViewWidth };
		std::uint32_t         height{ kDefaultViewHeight };

		std::string texture; // Optional extension-owned material binding metadata.
		bool fullscreen{ false }; // World-only: admits Fullscreen surface interactions.
		std::string browserChrome; // Reserved browser surface only.
		std::uint32_t browserChromeHeight{56};
		ViewKind kind{ ViewKind::Menu };  // "menu" | "hud"

		// Menu-only; forced false for HUDs.
		bool capturesInput{ true };
		// Forced false for HUDs.
		bool pausesGame{ true };

		// HUD-only autostart; debug HUDs require developer mode. Ignored for menus.
		bool openOnStart{ false };

		// HUD-only; clamped to 0..999.
		std::int32_t order{ 0 };

		// Hidden unless developer mode was enabled at startup.
		bool debugOnly{ false };
		bool legacy{ false }; // Uses the isolated OSF UI 1.6 asset root and web facade.

		// Parses a_path; returns std::nullopt and logs on any validation failure.
		static std::optional<ViewManifest> Load(const std::filesystem::path& a_path);
		static std::optional<ViewManifest> Parse(const std::filesystem::path& a_path, const nlohmann::json& a_document);
	};
}
