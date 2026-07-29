#include "core/Plugin.h"

SFSE_PLUGIN_PRELOAD(const SFSE::PreLoadInterface* a_sfse)
{
	// SFSE::Init initializes REX logging, so it must be the first statement —
	// no log call may precede it. Open at Debug so nothing before config load is
	// dropped; Log::SetDevMode raises the floor to Info once config is read
	// (Debug stays only when devMode is on). spdlog flushes at the active level,
	// so what we keep survives a crash that never flushes.
	// logRotate = 1 keeps the previous session as "OSF UI.1.log" — a crash log
	// must survive the next launch or the report prompt has nothing to attach.
	// The pattern adds the date so logs from different days aren't conflated.
	SFSE::Init(a_sfse, {
						   .logLevel = REX::ELogLevel::Debug,
						   .logPattern = "[%m-%d %T.%e] [%=5t] [%L] %v",
						   .logRotate = 1,
					   });

	return OSFUI::Plugin::OnPreLoad();
}

SFSE_PLUGIN_LOAD(const SFSE::LoadInterface* a_sfse)
{
	// Trampoline: MainThreadMenuPump patches the two UI_AdvanceActiveMenus
	// call sites (write_call<5> stubs); 256 bytes is ample headroom.
	SFSE::Init(a_sfse, { .trampoline = true, .trampolineSize = 256 });

	return OSFUI::Plugin::OnLoad();
}
