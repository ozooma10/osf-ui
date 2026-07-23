#pragma once

namespace OSFUI
{
	// Sim pause (OSF RE module ui.menu_pause, closed 2026-07-02). How the engine
	// pauses:
	//   - Main::isGameMenuPaused (Main+0x448) is a read-only output. Main::Update
	//     recomputes it every frame as
	//       (UI::pauseRequestCount > 0) || IsOpen("MainMenu") || g_145FB4B78
	//     right before the sim aggregator reads it, so no foreign byte write
	//     survives (log-proven: the first SimPause lost that write-war).
	//   - Native menus with IMenu flag bit 1 — the real kPausesGame; bit 27 is
	//     the letterbox latch — are counted in/out of UI::pauseRequestCount
	//     (+0x4B4) by the open/close dispatch via UI_ModifyMenuPauseCounter.
	// So: call UI::ModifyMenuPauseCounter(name, true/false) and let the engine
	// derive the byte. Live-proven over repeated freeze/resume cycles, gameHour
	// bit-identical while frozen, clean resume, no letterbox. Calls here are
	// strictly balanced and edge-triggered; a leaked increment pauses the game
	// forever.
	//
	// Unproven: quickload regression. The pause-released path entered a load
	// cleanly, but that probe session died to an unrelated silent load-crash;
	// one quickload in a stable session settles it.
	class SimPause
	{
	public:
		// Drive the engine pause-request counter toward a_desired. Call every
		// main-thread Tick; edges are detected internally. No-ops until the UI
		// singleton exists.
		static void Apply(bool a_desired);
	};
}
