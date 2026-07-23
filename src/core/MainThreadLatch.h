#pragma once

#include <atomic>

namespace OSFUI
{
	// Level-triggered marshaller onto the engine main thread.
	//
	// Runtime::Tick runs on an off-main SFSE render-graph worker (proven
	// 2026-07-23 — see src/core/ThreadAffinityProbe.cpp), but several engine
	// counters/objects (UI::pauseRequestCount, MenuCursor::freeCursorRefCount,
	// BSInputEnableManager) are main-thread-owned and mutated non-atomically by
	// the engine. Touching them from Tick is a data race. This latch defers the
	// mutation onto the real main thread via RE::BSService::TaskQueue, which
	// drains on the game main thread every frame (see [[bsservice-taskqueue-main-
	// thread]] / lib/commonlibsf/include/RE/B/BSService.h).
	//
	// Usage: hold one static instance per subsystem and call Request() every tick
	// from Tick (any thread). It only posts when the desired level differs from
	// what was last committed, so the steady-state path is two atomic ops and no
	// allocation. `a_apply(want)` runs on the MAIN thread and returns true once it
	// has committed `want`; returning false (e.g. the engine singleton isn't ready
	// yet) leaves the latch un-committed so the next tick retries — preserving the
	// old "no-op until the singleton exists" behavior.
	class MainThreadLatch
	{
	public:
		using ApplyFn = bool (*)(bool a_want);

		void Request(bool a_desired, ApplyFn a_apply);

	private:
		std::atomic<bool> _desired{ false };  // written on Tick (worker)
		std::atomic<bool> _applied{ false };  // committed on the main thread
		std::atomic<bool> _pending{ false };  // a post is queued/running — at most one in flight
	};
}
