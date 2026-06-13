#pragma once

// Small, isolated Win32 helpers. Keep all direct Win32 usage in this file pair
// so the rest of the codebase stays platform-call free.

namespace SWUI::Platform
{
	// Full path of the DLL this code is compiled into (not the host EXE).
	// Returns an empty path on failure.
	[[nodiscard]] std::filesystem::path GetThisModulePath();

	// Loads a DLL from an absolute path so that its own same-directory
	// dependencies resolve (LOAD_WITH_ALTERED_SEARCH_PATH). Needed because
	// SFSE loads plugins with plain LoadLibrary: a plugin's dependencies do
	// NOT resolve from the plugin's folder, only from the game EXE dir/PATH.
	// Returns false and sets a_lastError (GetLastError) on failure.
	bool LoadLibraryAbsolute(const std::filesystem::path& a_path, std::uint32_t& a_lastError);
}
