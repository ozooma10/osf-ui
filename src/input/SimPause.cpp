#include "input/SimPause.h"

#include "RE/B/BSFixedString.h"
#include "RE/U/UI.h"

#include "core/Log.h"
#include "core/MainThreadLatch.h"

namespace OSFUI
{
	namespace
	{
		// UI::pauseRequestCount is main-thread-owned and mutated non-atomically by
		// the engine, but Runtime::Tick runs on an off-main worker (proven
		// 2026-07-23), so the counter touch is marshalled onto the main thread by
		// this latch. Our contribution must stay strictly balanced — a leaked
		// increment pauses the game forever — which the latch guarantees: it
		// commits at most one engage/release transition, on the main thread.
		MainThreadLatch g_latch;

		// Name the engine's pause-counter bookkeeping records for us.
		const RE::BSFixedString& PauseSourceName()
		{
			static const RE::BSFixedString name{ "OSFUI_SimPause" };
			return name;
		}

		// Runs on the game MAIN thread (via g_latch). Returns false to defer when
		// the UI singleton isn't up yet, so Request retries next tick.
		bool ApplyOnMain(bool a_engage)
		{
			auto* ui = RE::UI::GetSingleton();
			if (!ui) {
				// Too early (boot); retry, warning once so it isn't silent. A
				// release edge cannot strand a count: an increment can only come
				// through this path, which needed the singleton.
				static bool warned = false;
				if (a_engage && !warned) {
					warned = true;
					REX::WARN("SimPause: pause requested but UI singleton is null; retrying every tick");
				}
				return false;
			}
			ui->ModifyMenuPauseCounter(PauseSourceName(), a_engage);
			REX::DEBUG("SimPause: {} (UI::pauseRequestCount {})", a_engage ? "engaged" : "released",
				a_engage ? "incremented" : "decremented");
			return true;
		}
	}

	void SimPause::Apply(bool a_desired)
	{
		g_latch.Request(a_desired, &ApplyOnMain);
	}

}
