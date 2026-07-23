#pragma once

namespace OSFUI
{
	// OS-cursor release without the engine's Scaleform arrow (OSF RE module
	// ui.menu_cursor, 2026-07-02). The per-frame windowing update (fn 99486)
	// ClipCursor's a zero-area rect at screen center while
	// MenuCursor::freeCursorRefCount is 0 (that clip is the gameplay
	// "recenter"), and ClipCursor(NULL)s while it is > 0. The only engine
	// writers of that counter are CursorMenu's kShow/kHide, which is why menu
	// flag bit 3 (ShowCursor) used to be load-bearing: bit 3 -> cursor
	// visibility driver -> CursorMenu -> {arrow + counter}. A plugin can take
	// the counter reference itself (game thread, non-atomic add, as the engine
	// does) and get the release without the arrow.
	//
	// With the focus menu's bit 3 clear, the visibility driver also kHides any
	// stray CursorMenu while our overlay is the decider menu, so the frozen
	// center arrow cannot appear or linger.
	//
	// DANGER: our +1 must be matched by exactly one -1. A leaked reference
	// leaves gameplay mouse-look without its cursor pin. Edge-triggered off one
	// flag, same discipline as SimPause.
	class FreeCursor
	{
	public:
		// Drive our reference on the free-cursor counter toward a_desired. Call
		// every tick from Tick (any thread); edges are detected internally and the
		// ref bump is marshalled onto the main thread via BSService::TaskQueue (see
		// core/MainThreadLatch), since Tick runs off-main. No-ops until the
		// MenuCursor singleton exists.
		static void Apply(bool a_desired);
	};
}
