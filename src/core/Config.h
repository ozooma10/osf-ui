#pragma once

namespace SWUI
{
	// Mirrors data/StarfieldWebUI/config.json. Unknown/missing/invalid fields
	// fall back to these defaults; a missing file is logged, not fatal.
	struct Config
	{
		bool        enabled{ true };
		std::string toggleKey{ "F10" };  // symbolic only — no key hook exists yet (see docs/reverse-engineering-notes.md)
		bool        startVisible{ false };
		std::string renderer{ "mock" };    // "null" | "mock" | "ultralight"
		std::string compositor{ "null" };  // "null" | "d3d12" (d3d12 is a stub)
		std::string inputSource{ "none" }; // "none" | "ui" (observe-only vfunc hook on RE::UI input processing)
		bool        captureInput{ true };  // when visible, route input to the web view and block the game from acting on it (needs inputSource="ui")
		std::string view{ "test" };
		bool        allowNetwork{ false };  // reserved; nothing implements network access
		bool        devMode{ false };  // release-safe default; the shipped config / a dev override turns on verbose logging

		// Loads from a_path; returns defaults (and logs why) on any failure.
		static Config Load(const std::filesystem::path& a_path);
	};
}
