#pragma once

namespace OSFUI::ScaleformOverlayHook
{
	// Engine-side hook for drawing under Starfield's native Scaleform UI.
	//
	// Starfield renders GFx movies into a transparent UI layer and later blends
	// it over the scene. Recording the browser quad at the ScaleformEnd hand-off
	// puts it upstream of both real-frame composition and Frame Generation.
	//
	// With FG active, the first RT->pixel-SRV candidate is an opaque interpolation
	// input, not the UI layer. The Scaleform hand-off shape identifies that graph and
	// skips the opaque candidate and writes only the transparent COPY_SOURCE
	// hand-off consumed by FFX.
	//
	// Pass executes run on a render-worker pool whose worker changes per frame,
	// so no engine resource or command list is retained across calls. Hooks fail
	// closed when a vtable slot does not hold the expected game implementation,
	// except for the explicitly proven Luma ScaleformComposite call-through
	// chain. There is no uninstall; process exit owns teardown.
	//
	// Installs the Begin/End/Composite hooks and enables the overlay draw. This is
	// the only path that puts the overlay on screen — the present-time renderer
	// was retired — so a false return means OSF UI cannot draw this session.
	bool Install();

	// Whether the Scaleform overlay can actually draw RIGHT NOW. Install()'s return value is
	// only the vtable-hook half; the command-list hooks are taken lazily on the
	// first frame from a render worker, and their self-test can fail (or a
	// later fault can disable the hook) long after Install() said yes. Callers
	// that gate on "can we put this on screen" must consult BOTH, or they admit
	// an invisible overlay that still captures input.
	[[nodiscard]] bool DrawEnabled();
}
