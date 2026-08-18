#pragma once

// Shared Win32 adapters for keyboard-layout and loaded-module facts.

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

	// Windows VK code -> DirectInput (DIK) scan code on the current keyboard
	// layout (MapVirtualKey VK_TO_VSC_EX; DIK codes are set-1 make codes with
	// 0x80 marking extended keys). 0 when untranslatable. Two callers: the
	// key-capture fallback when a message carries no scan code
	// (SendInput-synthesized input), and the one-time values migration that
	// re-anchors pre-2.x VK-based key names to physical scan codes.
	[[nodiscard]] std::uint32_t VkToDirectInputScan(std::uint32_t a_vk);

	// File NAME (not path) of the loaded module that owns a_address, or "" when
	// it cannot be resolved. Used to attribute an ABI-mismatched
	// OSFUI_RequestBridge call to the plugin that made it, so the refusal names
	// a DLL the player can go update. Never the full path — that identifies the
	// player's machine.
	[[nodiscard]] std::string ModuleNameForAddress(const void* a_address);
}
