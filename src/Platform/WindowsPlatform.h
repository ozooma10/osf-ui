#pragma once

// Win32 helpers. All direct Win32 usage lives in this file pair.

#include "Input/KeyLabels.h"

namespace OSFUI::Platform
{
	// Localized keycap labels for the layout of a_gameWindow's thread — the
	// thread whose WM_KEYDOWN stream capture and dispatch actually read (the
	// browser-host/Chromium process layout can drift independently; it is not the
	// authority). nullptr falls back to this thread's layout. The HKL is
	// re-read on every callable invocation, so a cached source stays correct
	// across layout switches.
	//
	// glyph: MapVirtualKeyExW(VSC->VK) + ToUnicodeEx with the
	// do-not-change-keyboard-state flag (0x4, Win10 1607+) so probing dead
	// keys (German ^ and ´) cannot corrupt an in-flight composition; a dead
	// key labels as its spacing accent. Single alphabetic glyphs are
	// uppercased to keycap form. layoutName: GetKeyNameTextW with the sided
	// "don't care" bit clear. layoutTag: LCIDToLocaleName ("de-DE").
	[[nodiscard]] KeyLabelSource MakeKeyLabelSource(void* a_gameWindow);

	// Full path of the DLL this code is compiled into (not the browser-host EXE).
	// Returns an empty path on failure.
	[[nodiscard]] std::filesystem::path GetThisModulePath();

	// The user's Documents folder (FOLDERID_Documents — follows OneDrive
	// redirection). Empty on failure. Base for persisted, writable data (e.g.
	// settings values), which cannot live under the read-only,
	// MO2/Program-Files-mapped plugin data folder.
	[[nodiscard]] std::filesystem::path GetDocumentsPath();

	// Windows VK code -> DirectInput (DIK) scan code on the current keyboard
	// layout (MapVirtualKey VK_TO_VSC_EX; DIK codes are set-1 make codes with
	// 0x80 marking extended keys). 0 when untranslatable. Two callers: the
	// key-capture fallback when a message carries no scan code
	// (SendInput-synthesized input), and the one-time values migration that
	// re-anchors pre-2.x VK-based key names to physical scan codes.
	[[nodiscard]] std::uint32_t VkToDirectInputScan(std::uint32_t a_vk);

	// True when [a_address, a_address + a_size) is committed, non-guard,
	// readable memory (VirtualQuery walk). For probing engine pointers.
	// File NAME (not path) of the loaded module that owns a_address, or "" when
	// it cannot be resolved. Used to attribute an ABI-mismatched
	// OSFUI_RequestBridge call to the plugin that made it, so the refusal names
	// a DLL the player can go update. Never the full path — that identifies the
	// player's machine (docs/security-model.md).
	[[nodiscard]] std::string ModuleNameForAddress(const void* a_address);

	[[nodiscard]] bool IsReadableRange(std::uintptr_t a_address, std::size_t a_size);

	// Reads one pointer-sized value if the location is readable.
	[[nodiscard]] bool SafeReadPointer(std::uintptr_t a_address, std::uintptr_t& a_value);
}
