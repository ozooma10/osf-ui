#pragma once

namespace OSFUI
{
	// Mirrors data/OSFUI/config.json, the developer/boot file: framework enable,
	// default view selection and developer mode. Mod-owned and
	// clobbered on update; holds no user-facing keys — those live in the `osfui`
	// settings schema (data/OSFUI/settings/osfui.json) and persist under
	// data/OSFUI/settings/values. Missing/invalid fields fall back to these
	// defaults; a missing file is logged, not fatal; unknown keys warn.
	struct Config
	{
		// Bumped only on a breaking config re-shape; a file written by a newer
		// OSF UI logs INFO and parses leniently. Removed fields have no special
		// compatibility handling; they are reported as unknown and ignored.
		static constexpr std::int64_t kConfigVersion = 2;

		bool        enabled{ true };
		// Mod Settings-owned toggle: not parsed from config.json — the `osfui` schema is
		// the sole owner and Runtime::OnSettingChanged mutates it live. It
		// doubles as the pre-replay boot default, so it MUST equal the schema
		// default.
		std::string toggleKey{ "F10" };  // key name -> physical scan code (ResolveKeyName); consumed by the WndProc hook
		// Inject a pauseMenuEntryLabel entry into the game's PauseMenu main list
		// at runtime (live Scaleform GFx manipulation — no SWF edit, no conflict
		// with UI-overhaul SWFs) and open pauseMenuEntryView when it is pressed.
		// The AS3 structure is decoded from the decompiled 1.16.244
		// pausemenu.swf. See input/PauseMenuEntry.h. Mod Settings owns the toggle;
		// the label/view
		// strings below stay dev knobs.
		bool        pauseMenuEntry{ true };  // Mod Settings-owned live state; not parsed from config.json
		std::string pauseMenuEntryLabel{ "MOD SETTINGS" };
		std::string pauseMenuEntryView{ "osfui/settings" };  // must be a discovered qualified view id, "<modId>/<viewName>"
		// Show warnings against the live engine ControlMap catalog. The catalog is
		// always published read-only; this Mod Settings-owned switch hides only warnings.
		bool        gameBindingWarnings{ true };  // persisted compatibility key: "vanillaKeyConflicts"
		std::string view{ "osfui/settings" };  // qualified "<modId>/<viewName>" id; the default menu the toggle key opens
		// Release-safe default; a dev override turns on verbose logging, view/schema
		// hot-reload, F12 DevTools — and lists third-party debugOnly views in the
		// mod menu.
		bool        devMode{ false };


		// Loads from a_path; returns defaults (and logs why) on any failure.
		static Config Load(const std::filesystem::path& a_path);
	};
}
