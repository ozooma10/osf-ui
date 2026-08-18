#pragma once

#include <cstdint>

namespace OSFUI
{
	// Logical (authoring) view size when a manifest omits width/height.
	inline constexpr std::uint32_t kDefaultViewWidth{ 1600 };
	inline constexpr std::uint32_t kDefaultViewHeight{ 900 };

	// Per-view permission grants; default denied, manifests opt in. Enforced at
	// the bridge/renderer boundary; filesystem and network grants are reserved.
	struct ViewPermissions
	{
		bool nativeBridge{ false };
		bool filesystem{ false };
		bool network{ false };
	};

	// menu = view eligible for the active-menu slot and input/pause policy.
	// hud = view presented over gameplay; never captures input.
	enum class ViewKind : std::uint8_t
	{
		Menu,
		Hud,
	};

	// Mirrors views/<modId>/<viewName>/manifest.json (api-freeze-plan item 1).
	struct ViewManifest
	{
		// Qualified view id "<modId>/<viewName>", derived entirely from the folder
		// path. A legacy manifest `id` field is accepted but ignored.
		std::string           id;
		std::string           title;
		std::string           description;  // one-line blurb for catalogs (`osfui/views` state / Mod Settings)
		// Owning mod id = the mod folder name under views/. Matches the settings
		// mod id (settings/<modId>.json / RegisterSettingsSchema) so Mod Settings
		// groups a mod's menu/HUD views onto its settings page.
		std::string           mod;
		std::string           entry{ "index.html" };
		// Logical (authoring) size; the page always lays out at this size. The
		// renderer resizes to output resolution with device scale
		// outputHeight/height, so CSS px scale up to output pixels.
		std::uint32_t         width{ kDefaultViewWidth };
		std::uint32_t         height{ kDefaultViewHeight };
		bool                  transparent{ true };
		bool                  menuInputEligible{ true };  // menu-kind capability summary; derived from kind and serialized as compatibility field `interactive`
		ViewPermissions       permissions;
		std::filesystem::path rootDir;  // directory containing the manifest

		ViewKind kind{ ViewKind::Menu };  // "menu" | "hud"

		// Menu-only: while this is the active menu, route input into the page. Forced false for HUDs.
		bool capturesInput{ true };
		// Menu-only: pause simulation while this is the active menu (engine
		// pause-request counter via Input/SimPause). Defaults true so menus pause
		// like native ones; a menu that wants the world running sets
		// "pausesGame": false. Forced false for HUDs.
		bool pausesGame{ true };

		// Menu: open at load. HUD: show at load.
		bool openOnStart{ false };

		// HUD-only within-band paint order for ViewPresentationController, clamped
		// 0..999; higher draws on top. Ignored for menus, which composite above
		// HUDs; only one menu is active at a time. The compositor's raw sort key comes from
		// the framework band, not from the manifest.
		std::int32_t order{ 0 };

		// List this view in catalogs (`osfui/views` state → the Mod Settings rail).
		// false = hidden utility view; still loads and works, just unadvertised.
		// Field name predates Mod Settings, kept for compatibility.
		bool catalogVisible{ true };  // serialized compatibility key: `hub`
		// Kept out of Mod Settings unless config.json devMode is on; still loads
		// and can be opened by id. Intended for developer-only tools.
		bool debugOnly{ false };

		// OSF UI release version this view was authored against ("1.2.0"). Advisory only,
		// never gates loading; when newer than the running OSF UI, Mod Settings
		// badges it "needs update". Empty when undeclared or malformed.
		std::string targetVersion;

		[[nodiscard]] std::filesystem::path EntryPath() const { return rootDir / entry; }

		// Parses a_path; returns std::nullopt and logs on any validation failure.
		static std::optional<ViewManifest> Load(const std::filesystem::path& a_path);
	};
}
