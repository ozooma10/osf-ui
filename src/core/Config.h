#pragma once

namespace OSFUI
{
	// Mirrors data/OSFUI/config.json, the developer/boot file: backends, input
	// source, diagnostic escape hatches, view set, dev knobs. Mod-owned and
	// clobbered on update; holds no user-facing keys — those live in the `osfui`
	// settings schema (data/OSFUI/settings/osfui.json) and persist under
	// data/OSFUI/settings/values. Missing/invalid fields fall back to these
	// defaults; a missing file is logged, not fatal; unknown keys warn.
	struct Config
	{
		// Bumped only on a breaking config re-shape; a file written by a newer
		// OSF UI logs INFO and parses leniently. v2 removed the central view
		// lists ('views'/'warmViews'): startup now follows manifest openOnStart
		// plus the player's per-HUD auto-start choices in Mod Settings.
		static constexpr std::int64_t kConfigVersion = 2;

		bool        enabled{ true };
		// MCM-owned toggle: not parsed from config.json — the `osfui` schema is
		// the sole owner and Runtime::OnSettingChanged mutates it live. It
		// doubles as the pre-replay boot default, so it MUST equal the schema
		// default.
		std::string toggleKey{ "F10" };  // key name -> Windows VK code (ResolveKeyName); consumed by the WndProc hook
		std::string inputSource{ "ui" };     // "none" | "ui" (WndProc subclass: toggle key + input capture; see input/OverlayInputHook)
		bool        captureInput{ true };  // when visible, route input to the web view and block the game from acting on it (needs inputSource="ui")
		// Show the Windows hardware pointer while the overlay captures input,
		// driven by absolute OS coordinates: zero-lag (composited on the
		// display's cursor plane, independent of game framerate), and the page's
		// CSS `cursor` maps to the matching system cursor. When false, falls back
		// to the raw-delta virtual cursor, which has no visible pointer (views
		// stopped drawing one) — debugging escape hatch only. Needs
		// inputSource="ui".
		bool        hardwareCursor{ true };
		// Register a real engine menu (OSFUI_FocusMenu) and open/close it with
		// the overlay so the engine enters menu mode (cursor ownership,
		// menu-stack presence, engine-routed input) instead of relying only on
		// the WndProc message-swallow. Teardown delegates kHide to the engine
		// base so the active-menu array stays consistent. See input/FocusMenu.h.
		bool        focusMenu{ true };
		// Level-2 engine-routed input: patch the focus menu's +0x10
		// BSInputEventUser vtable so the engine's per-menu input dispatch —
		// including gamepad, which the WndProc never sees — reaches the runtime.
		// Drained on the main thread (Runtime::DrainEngineInput) into web-view
		// navigation (D-pad/left-stick -> arrows, A -> Enter, B -> close,
		// right-stick -> scroll) plus raw `ui.gamepad` bridge events; a page can
		// own the gamepad wholesale via the `osfui.gamepadRaw` command.
		// Keyboard/mouse stay on the WndProc path (permanent hybrid). See
		// input/EngineInput.h.
		bool        engineInput{ true };
		// Inject a pauseMenuEntryLabel entry into the game's PauseMenu main list
		// at runtime (live Scaleform GFx manipulation — no SWF edit, no conflict
		// with UI-overhaul SWFs) and open pauseMenuEntryView when it is pressed.
		// The AS3 structure is decoded from the decompiled 1.16.244
		// pausemenu.swf. See input/PauseMenuEntry.h. MCM-owned; the label/view
		// strings below stay dev knobs.
		bool        pauseMenuEntry{ true };  // MCM-owned live state; not parsed from config.json
		std::string pauseMenuEntryLabel{ "MOD MENUS" };
		std::string pauseMenuEntryView{ "osfui/settings" };  // must be a discovered surface id, qualified "<mod>/<view>"
		// Include the game's own key bindings in the key-conflict data: curated
		// defaults from vanillakeys.json, overlaid by the controlmap text files
		// the engine honors. Informational only — warn, never block. MCM-owned;
		// toggles live (the table loads lazily on first enable).
		bool        vanillaKeyConflicts{ true };
		// MCM-owned kill switch for the consented diagnostic reporter (manual
		// System Health reports and the host's post-crash prompt — the latter
		// applies on the next launch, since the host process reads it at spawn).
		// The destination URL is compile-time (Version.h kBugReportEndpoint) and
		// deliberately not configurable.
		bool        bugReporting{ true };
		std::string view{ "osfui/settings" };  // qualified "<mod>/<view>" id; the default menu the toggle key opens
		// Release-safe default; a dev override turns on verbose logging, view/schema
		// hot-reload, F12 DevTools — and lists debugOnly views (e.g. the built-in
		// Web Performance Lab) in the mod menu.
		bool        devMode{ false };


		// Loads from a_path; returns defaults (and logs why) on any failure.
		static Config Load(const std::filesystem::path& a_path);
	};
}
