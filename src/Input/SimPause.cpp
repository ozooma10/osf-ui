#include "Input/SimPause.h"

#include "RE/B/BSFixedString.h"
#include "RE/U/UI.h"

#include "Core/Log.h"

namespace OSFUI
{
	namespace
	{
		// Runtime-owned; keep this owner's pause-counter contribution balanced.
		bool g_engaged{ false };

		const RE::BSFixedString& PauseSourceName()
		{
			// Leaked: the engine string table may be torn down before DLL detach.
			static auto* const name = new RE::BSFixedString("OSFUI_SimPause");
			return *name;
		}
	}

	void SimPause::Apply(bool a_desired)
	{
		if (a_desired == g_engaged) {
			return;
		}
		auto* ui = RE::UI::GetSingleton();
		if (!ui) {
			// Retry until RE::UI exists.
			static bool warned = false;
			if (a_desired && !warned) {
				warned = true;
				REX::WARN("SimPause: pause requested but UI singleton is null; retrying every tick");
			}
			return;
		}
		if (!a_desired && ui->pauseRequestCount == 0) {
			// Load teardown can reset the counter first; an unmatched decrement would wrap it.
			REX::WARN("SimPause: engine pause counter was already zero at release");
			g_engaged = false;
			return;
		}
		ui->ModifyMenuPauseCounter(PauseSourceName(), a_desired);
		g_engaged = a_desired;
		REX::DEBUG("SimPause: {} (UI::pauseRequestCount {})", a_desired ? "engaged" : "released",
			a_desired ? "incremented" : "decremented");
	}
}
