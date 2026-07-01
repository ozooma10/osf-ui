#pragma once

#include "render/IWebRenderer.h"

namespace OSFUI::HardwareCursor
{
	// Drives the REAL Windows (hardware) pointer while the overlay captures
	// input. The OS composites it on the display's cursor plane, outside the
	// game's render loop entirely, so it moves with zero perceived lag at any
	// game framerate — unlike anything drawn inside a view (a full Ultralight
	// paint + upload + Present behind the hand) or at Present time (one game
	// frame behind). Ultralight is built for this split: the page reports the
	// CSS cursor it wants (ViewListener::OnChangeCursor) and the host shows
	// the matching OS pointer.
	//
	// Threading: everything except SetShape must be called on the window-
	// message thread (from the WndProc hook) — the game manages cursor state
	// from that thread and ShowCursor's display counter is per-thread, so
	// raising/undoing it anywhere else would fight the wrong counter. SetShape
	// is thread-safe (the renderer's worker reports CSS cursor changes).
	//
	// a_hwnd parameters are the game HWND; typed void* to keep <Windows.h>
	// confined to the .cpp, matching the rest of src/input.

	// Make the pointer visible (bounded ShowCursor(TRUE) raises until the
	// counter reports shown), apply the current shape, park the pointer at the
	// window centre (mirrors the runtime's virtual-cursor recenter on open),
	// and clip it to the client area so it cannot wander onto another monitor
	// and click the game out of focus (the game pauses on focus loss).
	void Activate(void* a_hwnd);

	// Undo exactly our ShowCursor raises and release the clip. The game
	// re-asserts its own gameplay hide/clip on its own afterwards.
	void Deactivate();

	// Self-heal on every captured mouse message: the engine may re-hide or
	// re-clip the pointer at any time, and the next mouse message is exactly
	// when the user would notice. No-op when not activated.
	void Reassert(void* a_hwnd);

	// Apply the page's requested shape to the pointer (SetCursor). Called on
	// WM_SETCURSOR — the caller must then return TRUE so the game's WndProc
	// never resets it — and defensively from Reassert.
	void ApplyShape();

	// Record the latest shape the page asked for (CSS `cursor`); it is applied
	// on the next WM_SETCURSOR / mouse message. Thread-safe.
	void SetShape(CursorShape a_shape);
}
