#pragma once

#include <string>       // KeyName return type
#include <string_view>  // ResolveKeyName arg

namespace OSFUI
{
	// Keyboard key codes are **Windows virtual-key codes (VK_*)** — proven
	// in-game 2026-06-12: pressing F10 delivered ButtonEvent::idCode 121
	// (VK_F10) and left Alt delivered 164 (VK_LMENU). The earlier assumption
	// (DirectInput scan codes / SFSE InputMap space) was wrong for Starfield;
	// SFSE's InputMap is a macro-recording key space, not what UI ButtonEvents
	// carry. Mouse input never travels as KeyCode: the WndProc parses WM_INPUT
	// raw packets and routes buttons/wheel via Runtime::OnHostMouse* with a
	// plain button index (0=left, 1=right, 2=middle).
	using KeyCode = std::uint32_t;

	inline constexpr KeyCode kInvalidKeyCode = 0;

	// Resolves a config key name ("F10", "A", "Delete", ...) to a Windows
	// virtual-key code. F1-F24 and a small named-key table are
	// layout-independent; single letters/digits map to their VK values.
	// Returns kInvalidKeyCode and logs if the name cannot be resolved.
	[[nodiscard]] KeyCode ResolveKeyName(std::string_view a_name);

	// Reverse of ResolveKeyName: a VK code -> its canonical config name
	// ("F10", "A", "Delete", ...), for the settings key-rebind capture.
	// Returns an empty string if the VK has no canonical name (so the caller
	// can reject an unbindable key). Round-trips: ResolveKeyName(KeyName(vk))==vk.
	[[nodiscard]] std::string KeyName(KeyCode a_vk);
}
