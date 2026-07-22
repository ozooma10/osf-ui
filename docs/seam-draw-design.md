# Seam draw: overlay inside Starfield's UI render pass

Status: release default as of 2026-07-22. Validated in-game on Starfield
1.16.244 with built-in FSR3 Frame Generation, including opaque and translucent
content, loading, rapid mouse repaint, and FG activation.

## Why this path exists

Present-time D3D12 overlay drawing is unsafe once Frame Generation owns the
present chain. Starfield can present through multiple related swapchains, and a
foreign PRESENT-to-RT-to-PRESENT round trip on any of them can race FG queue
work and remove the device.

Starfield already has a UI path designed for real and generated frames. Drawing
the browser into that transparent UI layer gives both frame types the same
premultiplied composition and avoids vendor-specific FG integration.

## Validated frame-graph behavior

The relevant tail is:

scene/post-processing -> FG no-UI capture -> Scaleform UI subgraph ->
Frame Interpolation -> Scaleform composite -> present.

The UI subgraph ends at `ScaleformEnd`. Immediately after that pass, graph glue
hands off transient `R8G8B8A8_TYPELESS` resources while they are still in
`RENDER_TARGET` state. The seam records there, before the engine forwards the
handoff barrier.

Starfield's embedded FSR3 UI-composition pixel shader uses premultiplied over:

```text
output.rgb = ui.rgb + frame.rgb * (1 - ui.a)
output.a   = 1
```

The browser texture and seam PSO therefore remain premultiplied-alpha. The
straight-alpha and byte-comparator experiments used during diagnosis have been
removed from the release code.

## Target selection under Frame Generation

The first plausible RT-to-pixel-SRV handoff in the FG graph is not another UI
buffer. A frame-aligned byte capture proved it is an already-opaque scene image.
Writing the overlay there contaminated the interpolation input; FFX then applied
the transparent UI layer again. Opaque pixels hid the duplication, while
translucent pixels alternated between one and two blends.

The accepted rule is:

- FG off: draw at the normal RT-to-pixel-SRV UI handoff.
- FG on: skip the opaque pixel-SRV candidate and draw only at the transparent
  RT-to-COPY_SOURCE handoff consumed by FFX.

Present discovery publishes a short-lived FG-active signal to render workers.
Seeing the COPY_SOURCE handoff also latches the signal immediately, limiting a
late present classification to the activation boundary.

## Runtime contract

`UiPassSeam` hooks slot 7 on `ScaleformBegin`, `ScaleformEnd`, and
`ScaleformComposite`. Installation is fail-closed: every slot must still hold
the expected game implementation. A partial or foreign hook disables seam
drawing and leaves the legacy present renderer active.

Pass execution moves among render workers, so the implementation retains no
engine resource or command list across calls. The transient target is validated
by state, format, dimensions, and direct-list type at each handoff.

The compositor binds its own RTV, descriptor heap, root signature, and
premultiplied PSO, then restores the engine's tracked descriptor heaps. The
shared WebView texture ring uses the newest produce-fence-complete slot; if the
newest publication is incomplete, the seam reuses the last ready slot rather
than dropping the overlay for one frame.

In seam mode the Present hook performs plumbing only:

- output-size discovery;
- WebView shared-ring adoption;
- present liveness; and
- FG caller classification.

It never records a backbuffer draw.

## Defaults, fallback, and diagnostics

`uiPassDraw` defaults to `true`. Setting it to `false` temporarily selects the
legacy present renderer for compatibility diagnosis. That fallback still uses
`FrameGenActive()` suspension, so it intentionally becomes invisible rather
than risking a crash while FG is active.

`uiPassProbe` remains an off-by-default compatibility diagnostic for one release
cycle. When false, the seam keeps only the hook and draw work; the
`OMSetRenderTargets` capture hook, bounded capture windows, and characterization
logging do not run (the barrier/heaps hooks the draw itself needs stay in place).

The OSF RE sandbox UIPass experiment must remain disabled because it owns the
same vtable slots.

## Acceptance evidence

The final FSR3-FG run showed:

- the first overlay draw on the premultiplied FG UI input;
- all opaque scene candidates skipped afterward;
- stable opaque and translucent gradient regions across real/generated cadence;
- no mouse-repaint dropout;
- no device removal, crash, or present fallback; and
- load-safe, hitch-free behavior from the preceding seam acceptance sessions.

## Follow-up retirement

Keep the legacy present renderer for one release as a fail-closed compatibility
fallback. If supported game builds show no seam-install failures, remove its
backbuffer command allocators, RTVs, draw PSOs, and `FrameGenActive()` draw
suspension. Retain Present discovery only for output sizing, shared-ring
adoption, liveness, and FG target selection until a stronger engine FG state is
available.
