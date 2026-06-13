#pragma once

namespace SWUI
{
	// Mirrors data/StarfieldWebUI/config.json. Unknown/missing/invalid fields
	// fall back to these defaults; a missing file is logged, not fatal.
	struct Config
	{
		bool        enabled{ true };
		std::string toggleKey{ "F10" };  // symbolic only — no key hook exists yet (see docs/reverse-engineering-notes.md)
		std::string focusKey{ "Tab" };   // cycles the active (input) view when >1 interactive view is hosted
		bool        startVisible{ false };
		std::string renderer{ "mock" };    // "null" | "mock" | "ultralight"
		std::string compositor{ "null" };  // "null" | "d3d12" (d3d12 is a stub)
		std::string inputSource{ "none" }; // "none" | "ui" (observe-only vfunc hook on RE::UI input processing)
		bool        captureInput{ true };  // when visible, route input to the web view and block the game from acting on it (needs inputSource="ui")
		// EXPERIMENTAL (default off in code; shipped config.json turns it on).
		// Register a real engine menu (OSFUI_FocusMenu) and open/close it with
		// the overlay so the ENGINE enters menu mode (cursor + modal input
		// ownership) instead of relying only on the WndProc message-swallow.
		// Custom-IMenu registration + open IS proven on 1.16.244 (OSF RE
		// 2026-06-13-custom-imenu-registration); the headless-menu crash is
		// root-caused and the creator is hardened (engine base-init + engine
		// vtable copy + interned +0xB0 name). What is NOT yet live-validated is
		// that the hardened menu survives past the few-second mark the headless
		// one crashed at — this is the remaining risk when enabled. See
		// input/FocusMenu.h.
		bool        focusMenu{ false };
		// EXPERIMENTAL (default off in code; shipped config.json turns it on).
		// While the overlay is visible, disable player controls through the
		// engine input-enable layer (BSInputEnableManager) instead of only the
		// WndProc message-swallow — this also stops gamepad/XInput, which the
		// window hook never saw. PROVEN live on 1.16.244 with a controller
		// (keyboard + mouse-look + gamepad sticks all froze and restored cleanly;
		// OSF RE 2026-06-13-input-enable-layer-control-disable). See
		// input/ControlLayer.h.
		bool        disableControls{ false };
		std::string view{ "test" };
		// Optional multi-view set. When non-empty, every id is loaded and
		// composited together (layer order = each view's manifest `zorder`), and
		// `view` is the active (input) view — it must be an interactive view.
		// When empty, only `view` is loaded. Missing ids are skipped.
		std::vector<std::string> views;
		bool        allowNetwork{ false };  // reserved; nothing implements network access
		bool        devMode{ false };  // release-safe default; the shipped config / a dev override turns on verbose logging

		// Loads from a_path; returns defaults (and logs why) on any failure.
		static Config Load(const std::filesystem::path& a_path);
	};
}
