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

	// The user's Documents folder (FOLDERID_Documents — follows OneDrive
	// redirection). Empty on failure. Used as the base for persisted, writable
	// data (e.g. settings values), which must NOT live under the read-only,
	// MO2/Program-Files-mapped plugin data folder.
	[[nodiscard]] std::filesystem::path GetDocumentsPath();

	// True when [a_address, a_address + a_size) is committed, non-guard,
	// readable memory (VirtualQuery walk). For probing engine pointers.
	[[nodiscard]] bool IsReadableRange(std::uintptr_t a_address, std::size_t a_size);

	// Reads one pointer-sized value if the location is readable.
	[[nodiscard]] bool SafeReadPointer(std::uintptr_t a_address, std::uintptr_t& a_value);
}
