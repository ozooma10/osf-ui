#include "Core/Plugin.h"

SFSE_PLUGIN_PRELOAD(const SFSE::PreLoadInterface* a_sfse)
{
	SFSE::Init(a_sfse, {
		.logLevel = REX::ELogLevel::Debug,
		.logPattern = "[%m-%d %T.%e] [%=5t] [%L] %v",
		.logRotate = 1,
	});

	return true;
}

SFSE_PLUGIN_LOAD(const SFSE::LoadInterface* a_sfse)
{
	// Trampoline backs the UI_AdvanceActiveMenus call-site hook (two sites, one shared branch stub).
	SFSE::Init(a_sfse, { .trampoline = true, .trampolineSize = 256 });
	return OSFUI::Plugin::OnLoad();
}
