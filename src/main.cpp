#include "core/Plugin.h"

SFSE_PLUGIN_PRELOAD(const SFSE::PreLoadInterface* a_sfse)
{
	// SFSE::Init also initializes REX logging, so it must run before any log
	// call. Keep this the very first statement.
	SFSE::Init(a_sfse);

	return SWUI::Plugin::OnPreLoad();
}

SFSE_PLUGIN_LOAD(const SFSE::LoadInterface* a_sfse)
{
	SFSE::Init(a_sfse);

	return SWUI::Plugin::OnLoad();
}
