#pragma once

namespace OSFUI
{
	// Mirrors data/OSFUI/config.json — the DEVELOPER/boot file (api-freeze-plan
	// item 7): backends, input source, diagnostic escape hatches, view set, dev
	// knobs. It is mod-owned and clobbered on update; it holds NO user-facing
	// keys — those live in the `osfui` settings schema
	// (data/OSFUI/settings/osfui.json) and persist under
	// data/OSFUI/settings/values. Unknown/
	// missing/invalid fields fall back to these defaults; a missing file is
	// logged, not fatal; unknown keys WARN (host-owned file — a typo, item 8).
	struct Config
	{
		// Format stamp (item 8): bumped only on a breaking config re-shape; a
		// file written by a newer OSF UI logs INFO and parses leniently.
		static constexpr std::int64_t kConfigVersion = 1;

		bool        enabled{ true };
		// --- MCM-owned knobs (item 7). NOT parsed from config.json — the
		// `osfui` schema is the sole owner and Runtime::OnSettingChanged
		// mutates these fields live (they double as pre-replay boot defaults,
		// so they MUST equal the schema defaults).
		std::string toggleKey{ "F10" };  // key name -> Windows VK code (ResolveKeyName); consumed by the WndProc hook, verified in-game
		std::string renderer{ "mock" };    // "null" | "mock" | "webview2" (out-of-process host)
		std::string compositor{ "null" };  // "null" | "d3d12" (d3d12 draws the overlay at present time)
		std::string inputSource{ "none" }; // "none" | "ui" (WndProc subclass: toggle key + input capture; see input/OverlayInputHook)
		bool        captureInput{ true };  // when visible, route input to the web view and block the game from acting on it (needs inputSource="ui")
		// Show the REAL Windows (hardware) pointer while the overlay captures
		// input, driven by absolute OS coordinates: zero-lag (composited on the
		// display's cursor plane, independent of game framerate), and the page's
		// CSS `cursor` maps to the matching system cursor. When false, fall back
		// to the legacy raw-delta virtual cursor — which no longer has a visible
		// pointer (views stopped drawing one), so this is a debugging escape
		// hatch only. Needs inputSource="ui".
		bool        hardwareCursor{ true };
		// Register a real engine menu (OSFUI_FocusMenu) and open/close it with
		// the overlay so the ENGINE enters menu mode (cursor ownership + menu-
		// stack presence + engine-routed input) instead of relying only on the
		// WndProc message-swallow. On by default. Verified in-game on 1.16.244:
		// custom-IMenu registration, Route-A stack admission, long-session
		// survival, and the post-close teardown (kHide delegated to the engine
		// base so the active-menu array stays consistent) are all stable. See
		// input/FocusMenu.h.
		bool        focusMenu{ true };
		// Level-2 engine-routed input: patch the focus menu's +0x10
		// BSInputEventUser vtable so the engine's per-menu input dispatch —
		// including GAMEPAD, which the WndProc never sees — reaches the runtime.
		// Drained on the main thread (Runtime::DrainEngineInput) into web-view
		// navigation (D-pad/left-stick -> arrows, A -> Enter, B -> close,
		// right-stick -> scroll) plus raw `ui.gamepad` bridge events; a page can
		// own the gamepad wholesale via the `osfui.gamepadRaw` command. On by
		// default; delivery + routing verified in-game on 1.16.244 with a
		// controller. Keyboard/mouse stay on the WndProc path (the permanent
		// hybrid). See input/EngineInput.h.
		bool        engineInput{ true };
		// Inject a pauseMenuEntryLabel entry into the game's PauseMenu main
		// list at runtime (live Scaleform GFx manipulation — no SWF edit, no
		// conflict with UI-overhaul SWFs) and open pauseMenuEntryView when it
		// is pressed. The AS3 structure is decoded from the decompiled 1.16.244
		// pausemenu.swf. On by default; the inject + click round-trip is
		// verified in-game on 1.16.244. See input/PauseMenuEntry.h.
		// MCM-owned (item 7); the label/view strings below stay dev knobs.
		bool        pauseMenuEntry{ true };
		std::string pauseMenuEntryLabel{ "MOD MENUS" };
		std::string pauseMenuEntryView{ "osfui/settings" };  // must be a registered surface id (config.views), qualified "<mod>/<view>"
		// Include the game's own key bindings in the informational key-conflict
		// data (mcm-design §9 "vanilla hotkeys", v1): curated defaults from
		// vanillakeys.json, overlaid by the controlmap text files the engine
		// honors. Purely informational (warn, never block). MCM-owned (item 7);
		// toggles live (the table loads lazily on first enable).
		bool        vanillaKeyConflicts{ true };
		std::string view{ "osfui/settings" };  // qualified "<mod>/<view>" id
		// Optional multi-view set. When non-empty, every id is loaded and
		// composited together (layer order is set by the menu/HUD framework —
		// HUDs beneath open menus), and
		// `view` is the active (input) view — it must be an interactive view.
		// When empty, only `view` is loaded. Missing ids are skipped.
		std::vector<std::string> views;
		bool        devMode{ false };  // release-safe default; the shipped config / a dev override turns on verbose logging
		// Renderer-benchmark stats (docs/renderer-benchmark.md): periodic
		// "Bench:" lines with frame/tick/present/produce timing percentiles.
		// Dev knob for A/B measurement runs; off by default.
		bool        benchStats{ false };
		// Dev view-reload key (mcm-design.md §12.1): with devMode on, pressing
		// this reloads the top open menu's URL in place — the fast alt-tab
		// iteration loop for view assets (schema edits hot-reload on their
		// own). Consumed like the toggle key; empty disables. Ignored without
		// devMode, so shipping it in config.json is harmless for users.
		std::string devReloadKey{ "F11" };

		// Loads from a_path; returns defaults (and logs why) on any failure.
		static Config Load(const std::filesystem::path& a_path);
	};
}
