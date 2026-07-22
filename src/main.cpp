#include "core/Plugin.h"

SFSE_PLUGIN_PRELOAD(const SFSE::PreLoadInterface* a_sfse)
{
	// SFSE::Init initializes REX logging, so it must be the first statement —
	// no log call may precede it. Open at Debug so nothing before config load is
	// dropped; Log::SetDevMode raises the floor to Info once config is read
	// (Debug stays only when devMode is on). spdlog flushes at the active level,
	// so what we keep survives a crash that never flushes.
	SFSE::Init(a_sfse, { .logLevel = REX::ELogLevel::Debug });

	return OSFUI::Plugin::OnPreLoad();
}

SFSE_PLUGIN_LOAD(const SFSE::LoadInterface* a_sfse)
{
	SFSE::Init(a_sfse);

	return OSFUI::Plugin::OnLoad();
}
