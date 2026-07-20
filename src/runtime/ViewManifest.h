#pragma once

#include <cstdint>

namespace OSFUI
{
	// Logical (authoring) view size when a manifest omits width/height.
	inline constexpr std::uint32_t kDefaultViewWidth{ 1600 };
	inline constexpr std::uint32_t kDefaultViewHeight{ 900 };

	// Per-view permission grants; default denied, manifests opt in. Enforced at
	// the bridge/renderer boundary — filesystem/network have nothing to enforce
	// them yet (docs/security-model.md).
	struct ViewPermissions
	{
		bool nativeBridge{ false };
		bool filesystem{ false };
		bool network{ false };
	};

	// menu = modal overlay (can capture input, becomes the active view).
	// hud = passive overlay drawn over gameplay; never captures input.
	enum class SurfaceKind : std::uint8_t
	{
		Menu,
		Hud,
	};

	// Mirrors views/<modId>/<viewName>/manifest.json (api-freeze-plan item 1).
	struct ViewManifest
	{
		// Qualified view id "<modId>/<viewName>", derived from the folder path,
		// not the file. The manifest's `id` field must equal the view folder
		// name, but is only a consistency check.
		std::string           id;
		std::string           title;
		std::string           description;  // one-line blurb for catalogs (views.data / the Mods surface)
		// Owning mod id = the mod folder name under views/. Matches the settings
		// mod id (settings/<modId>.json / RegisterSettingsSchema) so the Mods
		// surface groups a mod's terminals/HUDs onto its settings page.
		std::string           mod;
		std::string           entry{ "index.html" };
		// Logical (authoring) size; the page always lays out at this size. The
		// renderer resizes to output resolution with device scale
		// outputHeight/height, so CSS px scale up to output pixels.
		std::uint32_t         width{ kDefaultViewWidth };
		std::uint32_t         height{ kDefaultViewHeight };
		bool                  transparent{ true };
		bool                  interactive{ true };  // may receive input and become the active (focused) view. Derived from kind (menu=true, hud=false); not a manifest field
		ViewPermissions       permissions;
		std::filesystem::path rootDir;  // directory containing the manifest

		SurfaceKind kind{ SurfaceKind::Menu };  // "menu" | "hud"

		// Menu-only: while this is the top open menu, freeze the game and route input into the page. Forced false for HUDs.
		bool capturesInput{ true };
		// Menu-only: pause simulation while this is the top open menu (engine
		// pause-request counter via input/SimPause). Defaults true so menus pause
		// like native ones; a menu that wants the world running sets
		// "pausesGame": false. Forced false for HUDs.
		bool pausesGame{ true };

		// Menu: open at load. HUD: show at load.
		bool openOnStart{ false };

		// HUD-only within-band paint order for the MenuController, clamped
		// 0..999; higher draws on top. Ignored for menus, which composite above
		// HUDs and stack by open order. The compositor's raw sort key comes from
		// the framework band, not from the manifest.
		std::int32_t order{ 0 };

		// List this view in catalogs (views.data → the Mods surface rail).
		// false = hidden utility view; still loads and works, just unadvertised.
		// Field name predates the Mods surface, kept for compat.
		bool hub{ true };

		// OSF UI version this view was authored against ("1.2.0"). Advisory only,
		// never gates loading; when newer than the running OSF UI the Mods
		// surface badges it "needs update". Empty when undeclared or malformed.
		std::string targetVersion;

		[[nodiscard]] std::filesystem::path EntryPath() const { return rootDir / entry; }

		// Parses a_path; returns std::nullopt and logs on any validation failure.
		static std::optional<ViewManifest> Load(const std::filesystem::path& a_path);
	};
}
