#pragma once

namespace OSFUI
{
	// Always on, no config gate. Disables player controls through the engine's
	// input-enable system (RE::BSInputEnableManager / BSInputEnableLayer) while
	// the overlay owns input. Unlike the WndProc message-swallow
	// (input/OverlayInputHook), this gates upstream of all devices, so a gamepad
	// stick no longer drives the game with the overlay open — the WndProc only
	// ever saw keyboard/mouse window messages, never XInput.
	//
	// Mechanism proven on 1.16.244 statically and with a live controller run
	// (OSF RE module ui.input_enable_layer): AllocateNewLayer claims a pooled
	// layer with all events enabled, EnableUserEvent/EnableOtherEvent toggle its
	// per-event mask, and the engine recomputes the cached aggregate the input
	// fanout reads on each call. Keyboard, mouse-look and gamepad stick all stop.
	//
	// Release model: hold one layer for the whole session and toggle its mask
	// rather than DecRef-on-close. DecRef also works (this CommonLibSF copy
	// delegates to the engine release, ID 45194, so LayerFreed runs and controls
	// restore with no leak), but a session-held layer avoids re-allocating from
	// the fixed 100-slot pool on every open.
	//
	// The engine mutation must run on the game main thread, but Runtime drives
	// this from Tick, which runs on an off-main worker (proven 2026-07-23). So
	// Apply() marshals the engage/release onto the main thread via
	// BSService::TaskQueue (see core/MainThreadLatch). The manager singleton is
	// invalid at the main menu, so the deferred apply no-ops (warn-once) until
	// gameplay; the overlay force-hides there anyway.
	class ControlLayer
	{
	public:
		// Drive the control-disable layer toward a_engage. Call every tick from
		// Tick (any thread); edges are detected internally and the actual layer
		// alloc/toggle happens on the main thread. Disabling allocates the layer
		// on first use; the pooled layer is kept for reuse across toggles.
		static void Apply(bool a_engage);

	private:
		ControlLayer() = default;
	};
}
