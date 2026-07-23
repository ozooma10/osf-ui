#pragma once

// Thread-affinity probe (devMode only). Proves which OS thread each per-frame
// task source actually drains on, by sampling GetCurrentThreadId() and comparing
// against a known-main-thread anchor. Motivation: SFSE's TaskInterface is
// byte-identical to SKSE's but drains on a render-graph worker, not the game
// main thread (crash-stack-proven 2026-07-23); and the engine's own native
// RE::BSService::TaskQueue (a DIFFERENT queue) claims "game thread" without a
// thread-id proof. See src/input/MainThreadMenuPump.h and
// lib/commonlibsf/include/RE/B/BSService.h. Every hook here is read-only and
// no-ops entirely when devMode is off.

namespace OSFUI::ThreadProbe
{
	// Ground truth. Call from the main-loop UI hook (MainThreadMenuPump's
	// post-advance thunk), which runs on the thread that owns Scaleform/AS3 by
	// construction. Records the thread id once, then stays quiet.
	void NoteMainLoop();

	// Sample the SFSE TaskInterface drain thread. Call from FrameTickTask::Run.
	// Logs the first few samples (tid + comparison to the main loop + a one-shot
	// backtrace), then goes silent.
	void NoteSfseTask();

	// Post a delegate through the engine's native RE::BSService::TaskQueue and
	// sample the thread it actually runs on — a DIFFERENT queue than SFSE's.
	// Call from a non-drain thread (e.g. FrameTickTask::Run) so it can't mask
	// the real drain thread via the inline fallback. Posts a bounded number of
	// probes total, then stops.
	void ProbeEngineQueue();
}
