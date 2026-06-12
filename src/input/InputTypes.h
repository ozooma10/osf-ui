#pragma once

namespace SWUI
{
	enum class MouseButton
	{
		kLeft,
		kRight,
		kMiddle,
	};

	// Key codes use SFSE's InputMap space (SFSE/InputMap.h):
	//   0..255   keyboard (DirectInput scan codes, DIK_*)
	//   256..263 mouse buttons
	//   264..265 mouse wheel
	//   266..281 gamepad
	// This matches what Starfield ButtonEvents carry in IDEvent::idCode for
	// keyboard devices, so no translation layer is needed at the hook.
	using KeyCode = std::uint32_t;

	inline constexpr KeyCode kInvalidKeyCode = 0;

	// Resolves a config key name ("F10", "A", "Delete", ...) to an InputMap
	// keyboard code. F1-F12 resolve via DIK constants; everything else is
	// matched against SFSE::InputMap::GetKeyboardKeyName (note: those names
	// come from the active keyboard layout, so they are layout-dependent).
	// Returns kInvalidKeyCode and logs if the name cannot be resolved.
	[[nodiscard]] KeyCode ResolveKeyName(std::string_view a_name);
}
