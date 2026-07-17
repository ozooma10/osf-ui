#pragma once

namespace OSFUI
{
	// ALWAYS ON (no config gate — the freeze tracks the menu capture policy).
	// The gamepad-leak half of the de-jank work, complementary to FocusMenu.
	//
	// Disables player controls through the engine's own input-enable system
	// (RE::BSInputEnableManager / BSInputEnableLayer) while the overlay owns
	// input. Unlike the WndProc message-swallow (input/OverlayInputHook), this
	// gates input at a point upstream of ALL devices — so a gamepad stick no
	// longer drives the game while the overlay is open (the WndProc only ever saw
	// keyboard/mouse window messages, never XInput). This is the Starfield analog
	// of Prisma UI's ControlMap::ToggleControls(...).
	//
	// Mechanism is proven on 1.16.244 — statically and with a live controller run
	// (OSF RE module ui.input_enable_layer / Investigations/Requests/
	// 2026-06-13-input-enable-layer-control-disable.md): AllocateNewLayer claims a
	// pooled layer (all events enabled), EnableUserEvent/EnableOtherEvent toggle its
	// per-event mask, and the engine recomputes the cached aggregate (the effective
	// gate the input fanout reads) on each call. The live run confirmed keyboard,
	// mouse-look, AND a gamepad stick all stop driving gameplay.
	//
	// Release model: we hold ONE layer for the whole session and toggle its mask
	// (disable on Engage, re-enable on Release), rather than DecRef-on-close. DecRef
	// now works — this CommonLibSF copy's DecRef delegates to the engine release
	// (ID 45194), so LayerFreed runs and controls restore with no leak — but a
	// session-held layer is simpler than an alloc/free per overlay toggle and avoids
	// re-allocating from the fixed 100-slot pool on every open. Either is correct.
	//
	// All three calls MUST run on the game main thread (Runtime drives them from
	// Tick). The manager singleton is not valid at the main menu, so Engage()
	// no-ops (warn-once) until gameplay — fine, the overlay force-hides there.
	class ControlLayer
	{
	public:
		// Disable the gameplay control flags. Lazily allocates the layer on first
		// use. No-op if already engaged or if the manager is unavailable.
		static void Engage();

		// Re-enable the flags Engage() disabled (restores controls). No-op if not
		// engaged. The pooled layer is kept for reuse.
		static void Release();

		[[nodiscard]] static bool IsEngaged();

	private:
		ControlLayer() = default;
	};
}
