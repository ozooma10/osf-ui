#include "input/SimPause.h"

#include "RE/M/Main.h"

#include "core/Log.h"

namespace OSFUI
{
	namespace
	{
		// Main-thread only (driven from Runtime::Tick).
		bool g_engaged{ false };
		// The re-assert path can fire repeatedly if an engine writer keeps
		// clearing the byte (the +0x448 writer set is not fully traced); log it
		// once per engagement so a fight doesn't flood the log.
		bool g_reassertLogged{ false };
	}

	void SimPause::Apply(bool a_desired)
	{
		auto* main = RE::Main::GetSingleton();
		if (!main) {
			return;  // too early (boot / main menu); retry next tick
		}
		if (a_desired) {
			// Re-assert while engaged: native GameMenuBase menus post writes to
			// this same byte on their own open/close, and a native close while
			// our menu is up would silently unpause the world underneath it.
			if (!main->isGameMenuPaused) {
				main->isGameMenuPaused = true;
				if (!g_engaged) {
					REX::INFO("SimPause: engaged (Main isGameMenuPaused = 1)");
				} else if (!g_reassertLogged) {
					g_reassertLogged = true;
					REX::INFO("SimPause: re-asserted after an engine write cleared the pause byte (logged once per engagement)");
				}
			}
			g_engaged = true;
		} else if (g_engaged) {
			// Release once. If a native pause menu were open at this instant this
			// would clear its pause too — its own close job rewrites the byte,
			// and overlay-over-native-pause-menu is not a supported overlap.
			main->isGameMenuPaused = false;
			g_engaged = false;
			g_reassertLogged = false;
			REX::INFO("SimPause: released (isGameMenuPaused = 0)");
		}
	}

	bool SimPause::IsEngaged()
	{
		return g_engaged;
	}
}
