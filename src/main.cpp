#include "core/Plugin.h"

SFSE_PLUGIN_PRELOAD(const SFSE::PreLoadInterface* a_sfse)
{
	// SFSE::Init also initializes REX logging, so it must run before any log
	// call. Keep this the very first statement. Debug level while the project
	// is pre-Phase-1: DEBUG lines (tick heartbeat, menu events) are the
	// forensic record when the game dies without flushing, and spdlog flushes
	// at the configured level so every line hits disk immediately.
	SFSE::Init(a_sfse, { .logLevel = REX::ELogLevel::Debug });

	return OSFUI::Plugin::OnPreLoad();
}

SFSE_PLUGIN_LOAD(const SFSE::LoadInterface* a_sfse)
{
	SFSE::Init(a_sfse);

	return OSFUI::Plugin::OnLoad();
}
