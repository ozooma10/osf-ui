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
	// NB (design v2, docs/seam-draw-design.md): the Frame Interpolation node
	// consumes ScaleformCompositeBuffer BEFORE the composite pass runs, so the
	// FG-correct overlay draw goes into the buffer inside the UI subgraph (at
	// ScaleformEnd), NOT at the composite pass — composite-pass drawing would
	// reach real frames only and flicker at FG 2x.
	//
	// This module currently installs LOG-ONLY probe hooks (config `uiPassProbe`,
	// a dev knob, default off) on three pass vtables:
	//  - ScaleformBegin: arms a whole-UI capture WINDOW (this worker's
	//    OMSetRenderTargets/ResourceBarrier calls are logged from here until
	//    the composite returns — ScaleformCompositeBuffer's clear/transition
	//    barriers reveal its resource, format, and size) and, post-original,
	//    scans the Scaleform HAL fields.
	//  - ScaleformEnd: the future injection point — phase markers show which
	//    side of the buffer's RT->SRV transition it sits on.
	//  - ScaleformComposite: closes the window; logs the pass ABI plus a
	//    bounded scan of the engine command context for the underlying
	//    ID3D12GraphicsCommandList (runtime-proven so far: cmdCtx+0x60).
	// Each pass instance is also scanned once (cached D3D12 objects, +0xF0
	// graph-resource index). The scans exist to close phase 1b's unknowns; the
	// draw path comes after they are settled and stays in the compositor, not
	// here.
	//
	// Threading: pass Executes run on a render worker POOL (worker changes per
	// frame), so everything here is thread-agnostic and nothing engine-side is
	// cached across calls. Hooks fail closed: a slot that does not hold the
	// expected engine implementation (game patch, foreign hook) is left alone.
	// There is no uninstall — SFSE has no shutdown callback and the hooks must
	// tolerate teardown by process exit, like the rest of the runtime.
	// The FG UI-input target's draw treatment (config `uiPassFgMode`).
	enum class FgMode
	{
		kOff,       // don't draw into the FG UI input (generated frames lose the overlay)
		kPremul,    // premultiplied, same as the composite input
		kStraight,  // un-premultiplied (FSR3 default UI composition semantics)
	};

	// a_draw: additionally record the overlay into the engine's UI buffers at
	// the ScaleformEnd seam (config `uiPassDraw`).
	bool Install(bool a_draw = false, FgMode a_fgMode = FgMode::kStraight);
}
