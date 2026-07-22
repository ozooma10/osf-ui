#pragma once

namespace OSFUI::UiPassSeam
{
	// Engine-side seam for drawing under Starfield's native Scaleform UI.
	//
	// Starfield renders GFx movies into a transparent UI layer and later blends
	// it over the scene. Recording the browser quad at the ScaleformEnd hand-off
	// puts it upstream of both real-frame composition and Frame Generation.
	//
	// With FG active, the first RT->pixel-SRV candidate is an opaque interpolation
	// input, not the UI layer. Present discovery identifies that graph; the seam
	// skips the opaque candidate and writes only the transparent COPY_SOURCE
	// hand-off consumed by FFX.
	//
	// Pass executes run on a render-worker pool whose worker changes per frame,
	// so no engine resource or command list is retained across calls. Hooks fail
	// closed when a vtable slot does not hold the expected game implementation.
	// There is no uninstall; process exit owns teardown.
	//
	// Installs the Begin/End/Composite hooks. a_draw enables the release seam.
	// Returns false if any hook cannot be installed, allowing Runtime to retain
	// the legacy present path.
	bool Install(bool a_draw = true);
}
