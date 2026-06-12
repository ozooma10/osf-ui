#pragma once

// Small, isolated Win32 helpers. Keep all direct Win32 usage in this file pair
// so the rest of the codebase stays platform-call free.

namespace SWUI::Platform
{
	// Full path of the DLL this code is compiled into (not the host EXE).
	// Returns an empty path on failure.
	[[nodiscard]] std::filesystem::path GetThisModulePath();
}
