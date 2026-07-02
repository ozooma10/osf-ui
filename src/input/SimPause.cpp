#include "input/SimPause.h"

#include "RE/B/BSFixedString.h"
#include "RE/U/UI.h"

#include "core/Log.h"

namespace OSFUI
{
	namespace
	{
		// Main-thread only (driven from Runtime::Tick). g_engaged mirrors OUR
		// contribution to UI::pauseRequestCount — the calls must stay strictly
		// balanced (a leaked increment = game paused forever), which is why the
		// increment/decrement is edge-triggered off this flag and nothing else.
		bool g_engaged{ false };

		// The name the engine's pause-counter bookkeeping records for us.
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
			// Too early (boot); retry next tick. Warn once if a pause is being
			// requested so a resolution failure can't be silent. On the release
			// edge this cannot strand a count: an increment can only have
			// happened through this same path, which needed the singleton.
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
