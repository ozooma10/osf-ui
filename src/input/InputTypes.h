#pragma once

namespace SWUI
{
	enum class MouseButton
	{
		kLeft,
		kRight,
		kMiddle,
	};

	// Key codes are Windows virtual-key codes (VK_*) for now. This is a
	// provisional choice: once a real Starfield input event source is wired
	// (see docs/reverse-engineering-notes.md) the native event's own code
	// space may be adopted instead.
	using KeyCode = std::uint32_t;
}
