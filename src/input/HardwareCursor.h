#pragma once

#include "render/IWebRenderer.h"

namespace OSFUI::HardwareCursor
{
	// Drives the Windows hardware pointer while the overlay captures input. The
	// OS composites it on the display's cursor plane, outside the game's render
	// loop, so it has no perceived lag at any framerate — unlike anything drawn
	// in a view or at Present time (one game frame behind). The page reports the
	// CSS cursor it wants; the host shows the matching OS pointer.
	//
	// Threading: everything except SetShape must run on the window-message
	// thread (from the WndProc hook) — the game manages cursor state from there
	// and ShowCursor's display counter is per-thread, so raising/undoing it
	// elsewhere fights the wrong counter. SetShape is thread-safe (the
	// renderer's worker reports CSS cursor changes).
	//
	// a_hwnd is the game HWND; typed void* to keep <Windows.h> in the .cpp.

	// Show the pointer (bounded ShowCursor(TRUE) raises until the counter
	// reports shown), apply the current shape, park it at the window centre
	// (mirrors the runtime's virtual-cursor recenter on open), and clip it to
	// the client area so it cannot wander to another monitor and click the game
	// out of focus (the game pauses on focus loss).
	void Activate(void* a_hwnd);

	// Undo exactly our ShowCursor raises and release the clip. The game
	// re-asserts its own gameplay hide/clip afterwards.
	void Deactivate();

	// Self-heal on every captured mouse message: the engine may re-hide or
	// re-clip the pointer at any time. No-op when not activated.
	void Reassert(void* a_hwnd);

	// Apply the page's requested shape (SetCursor). Called on WM_SETCURSOR —
	// the caller must then return TRUE so the game's WndProc never resets it —
	// and defensively from Reassert.
	void ApplyShape();

	// Record the shape the page asked for (CSS `cursor`); applied on the next
	// WM_SETCURSOR / mouse message. Thread-safe.
	void SetShape(CursorShape a_shape);
}
