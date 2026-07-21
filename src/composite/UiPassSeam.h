#pragma once

namespace OSFUI::UiPassSeam
{
	// The engine-side seam for drawing UNDER the native Scaleform UI.
	//
	// Starfield renders all GFx movies into an offscreen target
	// ("ScaleformCompositeBuffer") inside the frame graph's UI subgraph, then a
	// late ScaleformCompositeRenderPass blends that buffer over the finished
	// scene (after Frame Interpolation, before the end-of-frame captures). A
	// draw recorded at the head of that composite pass therefore lands OVER the
	// complete scene but UNDER every native menu/HUD — and, unlike the
	// present-time overlay, inside the frame-generation pipeline's UI handling.
	// Evidence: OSF RE context module `rendering.ui_pass` (static + runtime
	// proven on 1.16.244, 2026-07-21; vtable slot-7 hook survived a full 8.5k
	// frame session).
	//
	// This module currently installs LOG-ONLY probe hooks (config `uiPassProbe`,
	// a dev knob, default off) on two pass vtables:
	//  - ScaleformBegin: after the original runs, the Scaleform HAL global
	//    carries the UI render-target info and the engine command context —
	//    scanned to settle ScaleformCompositeBuffer's format/size.
	//  - ScaleformComposite: the future injection point — logs the pass ABI
	//    plus a bounded scan of the engine command context for the underlying
	//    ID3D12GraphicsCommandList (the ctx type's internals are the one thing
	//    the RE pass left unresolved).
	// The scans exist to close those two unknowns; the draw path comes after
	// they are settled and stays in the compositor, not here.
	//
	// Threading: pass Executes run on a render worker POOL (worker changes per
	// frame), so everything here is thread-agnostic and nothing engine-side is
	// cached across calls. Hooks fail closed: a slot that does not hold the
	// expected engine implementation (game patch, foreign hook) is left alone.
	// There is no uninstall — SFSE has no shutdown callback and the hooks must
	// tolerate teardown by process exit, like the rest of the runtime.
	bool Install();
}
