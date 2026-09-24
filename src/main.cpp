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
	SFSE::Init(a_sfse);
	return OSFUI::Plugin::OnLoad();
}
