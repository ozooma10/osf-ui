#pragma once

namespace OSFUI
{
	// OS-cursor release WITHOUT the engine's Scaleform arrow (OSF RE module
	// ui.menu_cursor, answered 2026-07-02). How the engine pins/releases the
	// pointer: the per-frame windowing update (fn 99486) ClipCursor's a
	// zero-area rect at screen center while MenuCursor::freeCursorRefCount is
	// 0 (that clip alone is the gameplay "recenter"), and ClipCursor(NULL)s
	// while it is > 0. The ONLY engine writers of that counter are
	// CursorMenu's kShow/kHide — which is why menu flag bit 3 (ShowCursor)
	// used to be load-bearing here: bit 3 -> cursor-visibility driver ->
	// CursorMenu -> {arrow + counter}, fusing the release to the arrow.
	// A plugin can take the counter reference itself (game thread, non-atomic
	// add, exactly like the engine) and get the release with NO arrow.
	//
	// With the focus menu's bit 3 now clear, the visibility driver also
	// actively kHides any stray CursorMenu while our overlay is the decider
	// menu — the frozen center arrow cannot appear or linger.
	//
	// Balance is sacred: our +1 must always be matched by exactly one -1
	// (a leaked reference would leave gameplay mouse-look without its cursor
	// pin). Edge-triggered off one flag, same discipline as SimPause.
	class FreeCursor
	{
	public:
		// Drive our reference on the free-cursor counter toward a_desired.
		// Main thread only; call every tick (edges detected internally).
		// No-ops until the MenuCursor singleton exists.
		static void Apply(bool a_desired);

		[[nodiscard]] static bool IsEngaged();
	};
}
