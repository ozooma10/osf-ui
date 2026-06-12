#pragma once

// The one (deliberate, isolated) hook in this project so far: a vfunc swap on
// RE::UI's BSInputEventReceiver::PerformInputProcessing, the point where the
// game's UI receives the per-frame input event queue.
//
// Why this is acceptable under the "no invented addresses" rule:
//  - the vtable location comes from CommonLibSF's maintained AddressLib IDs
//    (RE::UI::VTABLE[0]), not from anything guessed here;
//  - the thunk is OBSERVE-ONLY: it reads the event list and always calls the
//    original with the unmodified queue. It never consumes, filters, or
//    injects events, so game behavior is unchanged.
// Consuming input while the overlay has focus is Phase 4 and needs separate
// RE (see docs/reverse-engineering-notes.md §3).
//
// Disabled by default (config "inputSource": "none") until verified in-game.
// Installation is one-way: the vfunc cannot be safely restored once other
// overlays may have chained onto it, so there is no Uninstall(); a disabled
// flag makes the thunk pure pass-through instead.

namespace SWUI
{
	class UiInputHook
	{
	public:
		// Swaps the vfunc. Call once the UI singleton exists
		// (kPostPostDataLoad). Safe to call only once. Returns false and logs
		// if the UI singleton is missing.
		static bool Install();

		// Makes the thunk pass-through without unhooking.
		static void SetEnabled(bool a_enabled);
	};
}
