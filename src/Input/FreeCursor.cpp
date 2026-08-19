#include "Input/FreeCursor.h"

#include "RE/M/MenuCursor.h"

#include "Core/Log.h"

namespace OSFUI
{
	namespace
	{
		// Main-thread only; balance this owner's free-cursor reference.
		bool g_engaged{ false };
	}

	void FreeCursor::Apply(bool a_desired)
	{
		if (a_desired == g_engaged) {
			return;
		}
		auto* cursor = RE::MenuCursor::GetSingleton();
		if (!cursor) {
			// Retry at boot; no reference can be stranded before the singleton exists.
			static bool warned = false;
			if (a_desired && !warned) {
				warned = true;
				REX::WARN("FreeCursor: free cursor requested but MenuCursor singleton is null; retrying every tick");
			}
			return;
		}
		if (a_desired) {
			++cursor->freeCursorRefCount;
		} else if (cursor->freeCursorRefCount > 0) {
			// Never drive the engine counter negative after external resets.
			--cursor->freeCursorRefCount;
		}
		g_engaged = a_desired;
		REX::DEBUG("FreeCursor: {} (MenuCursor freeCursorRefCount now {})",
			a_desired ? "engaged" : "released", cursor->freeCursorRefCount);
	}
}
