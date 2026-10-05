#pragma once

#include <cstdint>

namespace OSFUI::HardwareCursor
{
	// Window thread only, except SetSystemCursorId.

	// Show, center, and clip the pointer to the client area.
	void Activate(void* a_hwnd);

	// Undoes only our ShowCursor raises and clip.
	void Deactivate();

	[[nodiscard]] bool IsActive();

	// Restore cursor visibility and clipping when the engine changes them.
	void Reassert(void* a_hwnd);

	// On WM_SETCURSOR; caller returns TRUE so the engine doesn't reset it.
	void ApplyShape();

	// Any thread.
	void SetSystemCursorId(std::uint32_t a_id);
}
