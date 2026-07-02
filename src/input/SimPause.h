#pragma once

namespace OSFUI
{
	// Step-3 sim pause, the proven mechanism (OSF RE module ui.menu_pause,
	// closed 2026-07-02). How the engine actually pauses:
	//   - Main::isGameMenuPaused (Main+0x448) is READ-ONLY OUTPUT — Main::Update
	//     recomputes it EVERY frame as
	//       (UI::pauseRequestCount > 0) || IsOpen("MainMenu") || g_145FB4B78
	//     right before the sim aggregator reads it, so no foreign byte write can
	//     ever survive (the first SimPause implementation lost exactly that
	//     write-war, log-proven).
	//   - Native menus with IMenu flag bit 1 (the REAL kPausesGame; bit 27 is
	//     the letterbox latch and was misassigned for weeks) get counted in/out
	//     of UI::pauseRequestCount (+0x4B4) by the open/close dispatch via
	//     UI_ModifyMenuPauseCounter.
	// The sanctioned plugin recipe (live-proven: repeated freeze/resume cycles
	// in normal gameplay, gameHour bit-identical while frozen, clean resume, no
	// letterbox): call UI::ModifyMenuPauseCounter(name, true/false) and let the
	// engine derive the byte. This class does that with strictly balanced
	// edge-triggered calls — a leaked increment would pause the game forever.
	//
	// Still unproven (RE left-open): quickload regression — the pause-released
	// path entered a load cleanly but that probe session died to an unrelated
	// silent load-crash; one quickload in a stable session settles it.
	class SimPause
	{
	public:
		// Drive the engine pause-request counter toward a_desired. Main thread
		// only; call every tick (edges are detected internally). No-ops until
		// the UI singleton exists.
		static void Apply(bool a_desired);

		[[nodiscard]] static bool IsEngaged();
	};
}
