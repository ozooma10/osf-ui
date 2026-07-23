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
drawing, and since the present-time renderer has been retired that means the
overlay does not draw at all that session — so `Install()` failure is logged at
error level by both `UiPassSeam` and `Runtime`.

Pass execution moves among render workers, so the implementation retains no
engine resource or command list across calls. The transient target is validated
by state, format, dimensions, and direct-list type at each handoff.

The compositor binds its own RTV, descriptor heap, root signature, and
premultiplied PSO, then restores the engine's tracked descriptor heaps. This is
the known-good seam behavior from before `b8e3643`; root-signature and
pipeline-state interception is intentionally not part of the release path.

The shared WebView texture ring uses the newest produce-fence-complete slot; if
the newest publication is incomplete, the seam reuses the last ready slot rather
than dropping the overlay for one frame.

The Present hook performs plumbing only:

- output-size discovery;
- WebView shared-ring adoption;
- present liveness; and
- FG caller classification.

It records no draw of any kind, and holds no backbuffer, RTV, PSO, or command
allocator.

## Diagnostics

The seam is unconditional; there is no `uiPassDraw` switch and no present-time
fallback to select. A knob whose off position renders no UI is not a useful
compatibility control, and the seam's own fail-closed slot check already
declines to hook when another mod owns the vtable.

Two consequences of dropping the backbuffer draw are worth noting:

- HDR and `_SRGB` backbuffers no longer suppress the overlay. The seam renders
  through a typed `R8G8B8A8_UNORM` view onto the engine's UI buffer, so the
  swapchain's own format is no longer inspected.
- Frame Generation no longer suspends drawing. `FrameGenActive()` and its
  swapchain-format gating are gone; FG classification survives only to pick the
  correct UI hand-off.

`CompositorStats::busyWaits` and `droppedBusy` counted the retired draw ring and
now stay zero. The fields remain on the wire so the host diagnostics page needs
no version dance.

The `uiPassProbe` characterization diagnostic has been removed now that the
frame graph, FG target selection, and hand-off decode are baked into the seam.
Only the seam's own load-bearing hooks remain: the `ScaleformBegin`/`End`/
`Composite` slot-7 hooks and the `ResourceBarrier`/`SetDescriptorHeaps`
command-list hooks the draw needs (hand-off match plus engine heap restore).

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

## Retirement of the present-time renderer

Done. The backbuffer command allocators, upload ring, CPU staging buffer, RTV
heap, per-format draw PSOs, and `FrameGenActive()` draw suspension were removed;
Present discovery was retained for output sizing, shared-ring adoption, liveness,
and FG target selection, as planned. `D3D12Compositor.cpp` went from 2128 to
~1470 lines.

Remaining step: migrate those four discovery duties onto the seam itself and drop
the Present hook entirely. Each is reachable from there —
`a_buffer->GetDesc()` carries the UI buffer dimensions, `LocateEngineD3D12()`
gives a device for ring adoption without a present, and the seam already latches
FG from the COPY_SOURCE hand-off. The hook should stay until the seam-only build
has real in-game time, because losing present discovery also loses the only
signal that classifies an FG pacer *before* the first hand-off of a frame.
