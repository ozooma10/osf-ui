#pragma once

#include <cstdint>

namespace OSFUI
{
	// Logical (authoring) view size when a manifest omits width/height.
	inline constexpr std::uint32_t kDefaultViewWidth{ 1600 };
	inline constexpr std::uint32_t kDefaultViewHeight{ 900 };

	// Menus may own input/pause; HUDs render over gameplay without capture.
	enum class ViewKind : std::uint8_t
	{
		Menu,
		Hud,
	};

	// Mirrors OSF/UI/views/<modId>/<viewName>/manifest.json.
	struct ViewManifest
	{
		// Derive <modId>/<viewName> from the folder path.
		std::string           id;
		std::string           title;
		std::string           description;
		// Owning mod id is the views/ folder name.
		std::string           mod;
		std::string           entry{ "index.html" };
		// Logical authoring size; the renderer scales CSS pixels to output height.
		std::uint32_t         width{ kDefaultViewWidth };
		std::uint32_t         height{ kDefaultViewHeight };
		bool                  transparent{ true };
		bool                  menuInputEligible{ true };  // derived from kind
		std::filesystem::path rootDir;  // directory containing the manifest

		ViewKind kind{ ViewKind::Menu };  // "menu" | "hud"

		// Menu-only: while this is the active menu, route input into the page. Forced false for HUDs.
		bool capturesInput{ true };
		// Menus pause by default through SimPause; HUDs force this false.
		bool pausesGame{ true };

		// Menu: open at load. HUD: show at load.
		bool openOnStart{ false };

		// HUD-only order is clamped to 0..999 within the framework-owned band.
		std::int32_t order{ 0 };

		// Hide local tools outside restart-latched developer mode.
		bool debugOnly{ false };

		[[nodiscard]] std::filesystem::path EntryPath() const { return rootDir / entry; }

		// Parses a_path; returns std::nullopt and logs on any validation failure.
		static std::optional<ViewManifest> Load(const std::filesystem::path& a_path);
	};
}
