#include "Input/SimPause.h"

#include "RE/B/BSFixedString.h"
#include "RE/U/UI.h"

#include "Core/Log.h"

namespace OSFUI
{
	namespace
	{
		// Main-thread only; keep this owner's pause-counter contribution balanced.
		bool g_engaged{ false };

		// Name the engine's pause-counter bookkeeping records for us.
		const RE::BSFixedString& PauseSourceName()
		{
			// The engine string table may already be tearing down at DLL detach.
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
			// Retry at boot; no count can be stranded before the singleton exists.
			static bool warned = false;
			if (a_desired && !warned) {
				warned = true;
				REX::WARN("SimPause: pause requested but UI singleton is null; retrying every tick");
			}
			return;
		}
		ui->ModifyMenuPauseCounter(PauseSourceName(), a_desired);
		g_engaged = a_desired;
		REX::DEBUG("SimPause: {} (UI::pauseRequestCount {})", a_desired ? "engaged" : "released",
			a_desired ? "incremented" : "decremented");
	}
}
