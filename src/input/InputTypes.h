#pragma once

namespace SWUI
{
	enum class MouseButton
	{
		kLeft,
		kRight,
		kMiddle,
	};

	// Keyboard key codes are **Windows virtual-key codes (VK_*)** — proven
	// in-game 2026-06-12: pressing F10 delivered ButtonEvent::idCode 121
	// (VK_F10) and left Alt delivered 164 (VK_LMENU). The earlier assumption
	// (DirectInput scan codes / SFSE InputMap space) was wrong for Starfield;
	// SFSE's InputMap is a macro-recording key space, not what UI ButtonEvents
	// carry. Mouse ButtonEvents carry their own small idCode space (observed:
	// 0 = left button); they are routed as MouseButton, never as KeyCode.
	using KeyCode = std::uint32_t;

	inline constexpr KeyCode kInvalidKeyCode = 0;

	// Resolves a config key name ("F10", "A", "Delete", ...) to a Windows
	// virtual-key code. F1-F24 and a small named-key table are
	// layout-independent; single letters/digits map to their VK values.
	// Returns kInvalidKeyCode and logs if the name cannot be resolved.
	[[nodiscard]] KeyCode ResolveKeyName(std::string_view a_name);
}
