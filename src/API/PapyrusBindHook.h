#pragma once

namespace OSFUI::API::Papyrus
{
	// Plugin load. Hooks GameVM's constructor so the OSFUI natives bind once, before any script runs.
	// Returns false when the call site does not match this game build; Install() then binds on data load instead.
	bool InstallBindHook();
}
