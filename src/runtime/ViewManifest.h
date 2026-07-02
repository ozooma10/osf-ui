#pragma once

namespace OSFUI
{
	// Per-view permission grants. Everything defaults to "denied"; manifests
	// opt in explicitly. Permissions are recorded now and enforced at the
	// bridge/renderer boundary (filesystem/network have no implementation to
	// enforce yet — see docs/security-model.md).
	struct ViewPermissions
	{
		bool nativeBridge{ false };
		bool filesystem{ false };
		bool network{ false };
	};

	//Type of menu. menu = modal overlay (capture input "active" view). hud = passive overlay (draws over gameplay, never captures input).
	enum class SurfaceKind : std::uint8_t
	{
		Menu,
		Hud,
	};

	// Mirrors views/<id>/manifest.json.
	struct ViewManifest
	{
		std::string           id;
		std::string           title;
		std::string           entry{ "index.html" };
		// Logical (authoring) size: the page always lays out at this size. The
		// renderer resizes views to output resolution with a matching device
		// scale (outputHeight/height), so CSS px scale up to output pixels.
		std::uint32_t         width{ 1280 };
		std::uint32_t         height{ 720 };
		bool                  transparent{ true };
		std::int32_t          zorder{ 0 };         // compositing layer; lower draws beneath, higher on top
		bool                  interactive{ true };  // may receive input and become the active (focused) view
		ViewPermissions       permissions;
		std::filesystem::path rootDir;  // directory containing the manifest

		SurfaceKind kind{ SurfaceKind::Menu };  // "menu" | "hud"

		// Menu-only: while this is the top open menu, freeze the game and route input into the page. Forced false for HUDs.
		bool capturesInput{ true };
		// Menu-only: pause simulation while this is the top open menu (engine
		// pause-request counter via input/SimPause). DEFAULT TRUE (2026-07-01,
		// by design: menus pause like native ones) — a menu that wants the world
		// to keep running sets "pausesGame": false. Forced false for HUDs.
		bool pausesGame{ true };

		// Menu: open at load. HUD: show at load.
		bool openOnStart{ false };

		// Within-band z ORDER HINT for the MenuController (HUD band vs menu band). Distinct from `zorder`, which is the raw runtime compositing sort key;
		std::int32_t order{ 0 };

		[[nodiscard]] std::filesystem::path EntryPath() const { return rootDir / entry; }

		// Parses a_path; returns std::nullopt and logs on any validation failure.
		static std::optional<ViewManifest> Load(const std::filesystem::path& a_path);
	};
}
