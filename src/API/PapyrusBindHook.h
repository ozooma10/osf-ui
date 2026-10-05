#pragma once

namespace OSFUI::API::Papyrus
{
	// Plugin load. Binds natives from GameVM's ctor before any script runs; false on call-site mismatch
	// (Install() then binds on data load).
	bool InstallBindHook();
}
