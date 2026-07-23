#include "input/FreeCursor.h"

#include "RE/M/MenuCursor.h"

#include "core/Log.h"
#include "core/MainThreadLatch.h"

namespace OSFUI
{
	namespace
	{
		// MenuCursor::freeCursorRefCount is main-thread-owned and bumped
		// non-atomically (as the engine does). Runtime::Tick runs on an off-main
		// worker (proven 2026-07-23), so the ref bump is marshalled onto the main
		// thread by this latch, which commits at most one balanced engage/release
		// transition — a leaked reference strands gameplay mouse-look.
		MainThreadLatch g_latch;

		// Runs on the game MAIN thread (via g_latch). Returns false to defer when
		// the MenuCursor singleton isn't up yet, so Request retries next tick.
		bool ApplyOnMain(bool a_engage)
		{
			auto* cursor = RE::MenuCursor::GetSingleton();
			if (!cursor) {
				// Too early (boot); retry. A release edge cannot strand a
				// reference: an increment could only have come through this same
				// path, which needed the singleton.
				static bool warned = false;
				if (a_engage && !warned) {
					warned = true;
					REX::WARN("FreeCursor: cursor release requested but MenuCursor singleton is null; retrying every tick");
				}
				return false;
			}
			if (a_engage) {
				++cursor->freeCursorRefCount;
			} else if (cursor->freeCursorRefCount > 0) {
				// Guarded like the engine's own decrement: don't drive it negative
				// if external state was perturbed (e.g. a load screen reset).
				--cursor->freeCursorRefCount;
			}
			REX::DEBUG("FreeCursor: {} (MenuCursor freeCursorRefCount now {})",
				a_engage ? "engaged" : "released", cursor->freeCursorRefCount);
			return true;
		}
	}

	void FreeCursor::Apply(bool a_desired)
	{
		g_latch.Request(a_desired, &ApplyOnMain);
	}

}
