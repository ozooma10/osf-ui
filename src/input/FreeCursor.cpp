#include "input/FreeCursor.h"

#include "RE/M/MenuCursor.h"

#include "core/Log.h"

namespace OSFUI
{
	namespace
	{
		// Main thread only (driven from Runtime::Tick). Mirrors our own
		// reference on MenuCursor::freeCursorRefCount; strictly balanced.
		bool g_engaged{ false };
	}

	void FreeCursor::Apply(bool a_desired)
	{
		if (a_desired == g_engaged) {
			return;
		}
		auto* cursor = RE::MenuCursor::GetSingleton();
		if (!cursor) {
			// Too early (boot); retry next tick. A release edge cannot strand a
			// reference: an increment could only have come through this same
			// path, which needed the singleton.
			static bool warned = false;
			if (a_desired && !warned) {
				warned = true;
				REX::WARN("FreeCursor: cursor release requested but MenuCursor singleton is null; retrying every tick");
			}
			return;
		}
		if (a_desired) {
			++cursor->freeCursorRefCount;
		} else if (cursor->freeCursorRefCount > 0) {
			// Guarded like the engine's own decrement: don't drive it negative
			// if external state was perturbed (e.g. a load screen reset).
			--cursor->freeCursorRefCount;
		}
		g_engaged = a_desired;
		REX::DEBUG("FreeCursor: {} (MenuCursor freeCursorRefCount now {})",
			a_desired ? "engaged" : "released", cursor->freeCursorRefCount);
	}

}
