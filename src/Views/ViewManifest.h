#pragma once

#include <cstdint>

namespace OSFUI
{
	// Logical (authoring) view size when a manifest omits width/height.
	inline constexpr std::uint32_t kDefaultViewWidth{ 1600 };
	inline constexpr std::uint32_t kDefaultViewHeight{ 900 };

	// Menus may own input/pause; HUDs and world materials are passive.
	enum class ViewKind : std::uint8_t
	{
		Menu,
		Hud,
		World,
	};

	// Mirrors OSF/UI/views/<modId>/<viewName>/manifest.json.
	struct ViewManifest
	{
		// Derive <modId>/<viewName> from the folder path.
		std::string           id;
		std::string           title;
		std::string           description;
		std::string           launcherMod; // Nonempty opts this menu into the shared launcher.
		std::string           launcherModTitle;
		// Owning mod id is the views/ folder name.
		std::string           mod;
		std::string           entry{ "index.html" };
		// Logical authoring size; the renderer scales CSS pixels to output height.
		std::uint32_t         width{ kDefaultViewWidth };
		std::uint32_t         height{ kDefaultViewHeight };
		bool                  menuInputEligible{ true };  // derived from kind

		ViewKind kind{ ViewKind::Menu };  // "menu" | "hud" | "world"
		// World-only: square BGRA8 material placeholder dimensions, before replacement.
		std::uint32_t placeholderSize{ 0 };

		// Menu-only: while this is the active menu, route input into the page.
		bool capturesInput{ true };
		// Menus pause by default through SimPause; passive views force this false.
		bool pausesGame{ true };

		// HUD: show at load. World views start when their material is discovered.
		bool openOnStart{ false };

		// HUD-only order is clamped to 0..999 within the framework-owned band.
		std::int32_t order{ 0 };

		// Hide local tools outside restart-latched developer mode.
		bool debugOnly{ false };

		// Parses a_path; returns std::nullopt and logs on any validation failure.
		static std::optional<ViewManifest> Load(const std::filesystem::path& a_path);
	};
}
