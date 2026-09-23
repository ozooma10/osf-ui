#pragma once

// Shared Win32 adapters for input state and loaded-module facts.

namespace OSFUI::Platform
{
	[[nodiscard]] bool AnyKeyDown();
	[[nodiscard]] bool GameIsForeground();

	// Return only the owning module's filename so diagnostics never expose the player's full path.
	[[nodiscard]] std::string ModuleNameForAddress(const void* a_address);
}
