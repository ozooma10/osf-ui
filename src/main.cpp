#include "core/Plugin.h"

SFSE_PLUGIN_PRELOAD(const SFSE::PreLoadInterface* a_sfse)
{
	// SFSE::Init initializes REX logging, so it must be the first statement —
	// no log call may precede it. Debug level pre-Phase-1: spdlog flushes at the
	// configured level, so the tick heartbeat and menu events hit disk
	// immediately and survive a crash that never flushes.
	SFSE::Init(a_sfse, { .logLevel = REX::ELogLevel::Debug });

	return OSFUI::Plugin::OnPreLoad();
}

SFSE_PLUGIN_LOAD(const SFSE::LoadInterface* a_sfse)
{
	SFSE::Init(a_sfse);

	return OSFUI::Plugin::OnLoad();
}
