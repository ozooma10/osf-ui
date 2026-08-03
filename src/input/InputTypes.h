#pragma once

#include <string>       // KeyName return type
#include <string_view>  // ResolveKeyName arg

namespace OSFUI
{
	// Keyboard key codes as DELIVERED by the window layer are Windows
	// virtual-key codes (VK_*), confirmed in-game 2026-06-12: F10 arrived as
	// ButtonEvent::idCode 121 (VK_F10), left Alt as 164 (VK_LMENU). Not the
	// SFSE InputMap space — InputMap is a macro-recording key space, unrelated
	// to what UI ButtonEvents carry. Mouse input never travels as a KeyCode:
	// the WndProc parses WM_INPUT raw packets and routes buttons/wheel via
	// Runtime::OnHostMouse* with a button index (0=left, 1=right, 2=middle).
	using KeyCode = std::uint32_t;

	inline constexpr KeyCode kInvalidKeyCode = 0;

	// Canonical BINDING identity: the physical key, as a set-1 make code in
	// the DirectInput convention (0x80 | base for 0xE0-prefixed extended keys)
	// — the same space the engine's controlmap hex tokens use. VKs are what
	// messages carry; layouts reassign them across physical keys (a German
	// layout puts VK_OEM_3 on the Ö key), so persisted bindings, hotkey
	// dispatch, and conflict grouping all live in ScanCode space instead. The
	// keycap a layout prints on a ScanCode is display data (KeyLabels), never
	// identity.
	using ScanCode = std::uint16_t;

	inline constexpr ScanCode kInvalidScanCode = 0;

	// Resolves a config key name ("F10", "A", "Delete", ...) to the physical
	// scan code. Names denote positions on the US reference keyboard, exactly
	// like the engine controlmap's DIK values; they are layout-independent.
	// Returns kInvalidScanCode and logs if the name cannot be resolved.
	[[nodiscard]] ScanCode ResolveKeyName(std::string_view a_name);

	// Reverse of ResolveKeyName: a scan code -> its canonical config name
	// ("F10", "A", "Delete", ...), for the settings key-rebind capture.
	// Returns an empty string if the code has no canonical name (so the caller
	// can reject an unbindable key). Round-trips: ResolveKeyName(KeyName(sc))==sc.
	[[nodiscard]] std::string KeyName(ScanCode a_scan);
}
