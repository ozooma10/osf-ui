#pragma once

#include <cstdint>

namespace OSFUI::HardwareCursor
{
	// Run cursor state on the window thread; only SetSystemCursorId is thread-safe for renderer callbacks.

	// Show, center, and clip the hardware pointer to the game client area.
	void Activate(void* a_hwnd);

	// Undo only our ShowCursor raises and clip, then let the game restore its state.
	void Deactivate();

	[[nodiscard]] bool IsActive();

	// Heal engine hide/clip changes on captured mouse messages.
	void Reassert(void* a_hwnd);

	// After applying the page cursor on WM_SETCURSOR, return TRUE to prevent engine reset.
	void ApplyShape();

	// Thread-safe: record WebView2's system cursor ID for the next window-thread application.
	void SetSystemCursorId(std::uint32_t a_id);
}
