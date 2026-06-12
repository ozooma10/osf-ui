#pragma once

// Entry-point glue between the SFSE plugin macros (src/main.cpp) and the
// Runtime. Keeps main.cpp trivial and the template entry pattern intact.

namespace SFSE
{
	class PreLoadInterface;
	class LoadInterface;
}

namespace SWUI::Plugin
{
	// Called from SFSE_PLUGIN_PRELOAD after SFSE::Init. Logging is already
	// live (SFSE::Init sets it up). Must not touch game state.
	bool OnPreLoad();

	// Called from SFSE_PLUGIN_LOAD after SFSE::Init. Builds and initializes
	// the Runtime. Returns false on fatal initialization failure.
	bool OnLoad();
}
