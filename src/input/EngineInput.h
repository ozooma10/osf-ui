#pragma once

namespace OSFUI
{
	// EXPERIMENTAL — Level-2 engine-routed input, increment 2 (config
	// `engineInput`): an OBSERVER-ONLY tap on the engine's per-menu input
	// dispatch. Proven contract (OSF RE module ui.menu_input, 1.16.244):
	// menus in the active array receive input through the BSInputEventUser
	// subobject at IMenu+0x10 — UI::PerformInputProcessing walks the array
	// top-down per event and dispatches by type to the receiver vtable slots
	// (1 ShouldHandleEvent, 4 thumbstick, 5 cursorMove, 6 mouseMove, 7 char,
	// 8 button; base slot 9 stays = held/release admission). Dispatch arrives
	// on a frame-worker THREAD POOL, so the thunks only bump atomic counters
	// and a small ring — no allocation, no game calls.
	//
	// Increment 2 does NOT route anything into the web view and does NOT mark
	// events handled: the WndProc path stays authoritative (no double input).
	// While the overlay captures, the WndProc swallow starves the engine of
	// keyboard/mouse, so the observer is expected to see GAMEPAD events only —
	// which is precisely the device the WndProc can never see, i.e. the point
	// of Level 2. Cutover (increment 3: inject from here, bit-4 firewall, drop
	// the game-facing swallow) happens only after this observer proves stable
	// delivery in-game.
	//
	// Stability note: the +0x10 vtable copy carries its RTTI COL at [-1] — the
	// same lesson as the primary-vtable copy (a COL-less copy AVs the first
	// time the engine dynamic_casts through it).
	class EngineInput
	{
	public:
		// Master switch, set once at init from config.engineInput.
		static void SetEnabled(bool a_enabled);
		[[nodiscard]] static bool IsEnabled();

		// Overwrite the +0x10 BSInputEventUser vptr of a freshly engine-built
		// focus-menu object with the patched copy. Called from the FocusMenu
		// creator; no-op unless enabled.
		static void InstallReceiver(void* a_menuObj);

		// One INFO line summarizing everything observed since the last call,
		// then reset. Runtime calls this on the focus-menu close edge so each
		// overlay session gets exactly one summary.
		static void LogSessionSummary();
	};
}
