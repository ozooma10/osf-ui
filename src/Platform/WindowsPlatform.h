#pragma once

// Shared Win32 adapters for input state and loaded-module facts.

namespace OSFUI::Platform
{

	// Return only the owning module's filename so diagnostics never expose the player's full path.
	[[nodiscard]] std::string ModuleNameForAddress(const void* a_address);
}
