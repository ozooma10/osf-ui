#pragma once

// Entry-point glue between the SFSE plugin macros in main.cpp and the Runtime.

namespace SFSE
{
	class PreLoadInterface;
	class LoadInterface;
}

namespace OSFUI::Plugin
{
	// Called from SFSE_PLUGIN_PRELOAD after SFSE::Init. Logging is already
	// live (SFSE::Init sets it up). Must not touch game state.
	bool OnPreLoad();

	// Called from SFSE_PLUGIN_LOAD after SFSE::Init. Builds and initializes
	// the Runtime. Returns false on fatal initialization failure.
	bool OnLoad();
}
