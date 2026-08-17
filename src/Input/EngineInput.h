#pragma once

namespace OSFUI
{
	// Engine-side gamepad capture gate installed on the focus menu's input
	// receiver. Controller routing itself is polled on the main thread; this
	// hook exists only to stop Starfield from acting on the same input.
	//
	// Contract (OSF RE module ui.menu_input, 1.16.244): menus in the active array
	// receive input through the BSInputEventUser subobject at IMenu+0x10.
	// UI::PerformInputProcessing walks the array top-down per event, dispatching
	// by type to receiver vtable slots (1 ShouldHandleEvent, 4 thumbstick,
	// 5 cursorMove, 6 mouseMove, 7 char, 8 button; base slot 9 stays =
	// held/release admission). Dispatch arrives on a frame-worker thread pool, so
	// the thunks only read an atomic flag and, when capturing, set gamepad event
	// status to kStop. They allocate nothing and make no game calls.
	//
	// Keyboard/mouse events are never marked handled, so WndProc stays
	// authoritative for them (no double input); while the overlay captures, the
	// WndProc swallow starves the engine of keyboard/mouse, so the tap sees
	// gamepad events only. GAMEPAD events are consumed while the overlay captures:
	// the receiver sets InputEvent::status = kStop so downstream receivers —
	// notably the player controls, which ignore the ControlLayer disable flags
	// for thumbstick movement — never act on them (the player walked around
	// under the open overlay otherwise). Verified in-game on 1.16.244 with a
	// controller (2026-07-02).
	//
	// The +0x10 vtable copy must carry its RTTI COL at [-1], as with the primary
	// vtable copy: a COL-less copy access-violates the first time the engine
	// dynamic_casts through it.
	class EngineInput
	{
	public:
		// Overwrite the +0x10 BSInputEventUser vptr of a freshly engine-built
		// focus-menu object with the patched copy. Called from the FocusMenu
		// creator.
		static void InstallReceiver(void* a_menuObj);

		static void SetGamepadCapture(bool a_capture);
	};
}
