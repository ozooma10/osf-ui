#pragma once

namespace OSFUI
{
	// Step-3 sim pause, the proven mechanism (OSF RE module ui.menu_pause,
	// 2026-07-02). "Menu pause" is TWO independent engine channels:
	//   A) the freeze-frame/letterbox latch — armed by IMenu flag bit 27, but
	//      only consulted for the top kModal menu on the open path; COSMETIC,
	//      live-proven NOT to stop the sim (the game calendar kept advancing
	//      under an engaged latch);
	//   B) the SIMULATION pause — Main::isGameMenuPaused (Main+0x448), one of
	//      three ORed pause bytes read by Main's per-frame aggregator;
	//      live-proven sufficient alone (world calendar froze while set).
	// Native menus reach B by CLASS, not flag: a MenuOpenCloseEvent sink
	// dynamic_casts the opened menu to GameMenuBase and posts its pause bool
	// through a job queue — a plain IMenu (our movie-less focus menu) can never
	// take that lane, no matter its flags. The sanctioned plugin recipe for
	// "pause without letterbox" is driving the byte directly, which is what
	// this does.
	//
	// Semantics: Apply() is reconciled every Tick on the game MAIN thread.
	// While engaged it RE-ASSERTS the byte (native GameMenuBase close jobs also
	// write it and would otherwise unpause the world underneath our menu); on
	// release it writes false once. NOT yet soak-proven by the RE probe pass
	// (pending): repeated freeze/resume cycles and quickload-while-paused —
	// treat the first in-game session as that verification.
	class SimPause
	{
	public:
		// Drive the sim-pause byte toward a_desired. Main thread only. Safe to
		// call every tick; no-ops (and stays disengaged) until the Main
		// singleton exists.
		static void Apply(bool a_desired);

		[[nodiscard]] static bool IsEngaged();
	};
}
