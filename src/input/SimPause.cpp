#include "input/SimPause.h"

#include "RE/B/BSFixedString.h"
#include "RE/U/UI.h"

#include "core/Log.h"

namespace OSFUI
{
	namespace
	{
		// Main thread only (driven from Runtime::Tick). Mirrors our own
		// contribution to UI::pauseRequestCount, which must stay strictly
		// balanced — a leaked increment pauses the game forever. Hence the
		// increment/decrement is edge-triggered off this flag and nothing else.
		bool g_engaged{ false };

		// Name the engine's pause-counter bookkeeping records for us.
		const RE::BSFixedString& PauseSourceName()
		{
			static const RE::BSFixedString name{ "OSFUI_SimPause" };
			return name;
		}
	}

	void SimPause::Apply(bool a_desired)
	{
		if (a_desired == g_engaged) {
			return;
		}
		auto* ui = RE::UI::GetSingleton();
		if (!ui) {
			// Too early (boot); retry next tick, warning once so a failure to
			// resolve isn't silent. The release edge cannot strand a count: an
			// increment can only come through this path, which needed the
			// singleton.
			static bool warned = false;
			if (a_desired && !warned) {
				warned = true;
				REX::WARN("SimPause: pause requested but UI singleton is null; retrying every tick");
			}
			return;
		}
		ui->ModifyMenuPauseCounter(PauseSourceName(), a_desired);
		g_engaged = a_desired;
		REX::INFO("SimPause: {} (UI::pauseRequestCount {})", a_desired ? "engaged" : "released",
			a_desired ? "incremented" : "decremented");
	}

	bool SimPause::IsEngaged()
	{
		return g_engaged;
	}
}
