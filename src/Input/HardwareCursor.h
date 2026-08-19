#pragma once

#include "Input/CursorShape.h"

namespace OSFUI::HardwareCursor
{
	// Run cursor state on the window thread; only SetShape is thread-safe for renderer callbacks.

	// Show, center, and clip the hardware pointer to the game client area.
	void Activate(void* a_hwnd);

	// Undo only our ShowCursor raises and clip, then let the game restore its state.
	void Deactivate();

	// Heal engine hide/clip changes on captured mouse messages.
	void Reassert(void* a_hwnd);

	// After applying the page cursor on WM_SETCURSOR, return TRUE to prevent engine reset.
	void ApplyShape();

	// Thread-safe: record the CSS cursor for the next window-thread application.
	void SetShape(CursorShape a_shape);
}
