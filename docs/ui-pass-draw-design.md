# UI-pass draw: overlay inside Starfield's UI render pass

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
`RENDER_TARGET` state. Luma upgrades the same resources to
`R16G16B16A16_FLOAT`; the draw accepts both and records through a matching typed
RTV and PSO before the engine forwards the handoff barrier.

Starfield's embedded FSR3 UI-composition pixel shader uses premultiplied over:

```text
output.rgb = ui.rgb + frame.rgb * (1 - ui.a)
output.a   = 1
```

The browser texture and overlay PSO therefore remain premultiplied-alpha. The
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

The UI-pass hook classifies its own frame graph. At the start of each render region it
uses whether the preceding region exposed the transparent COPY_SOURCE handoff;
seeing that handoff also latches FG immediately for the remainder of the current
region. The first region is observed before an ordinary target is used, avoiding
an initial double blend. An FG on/off transition can affect one boundary frame
because the normal candidate precedes the evidence that distinguishes the graph.

## Runtime contract

`UiPass` hooks slot 7 on `ScaleformBegin`, `ScaleformEnd`, and
`ScaleformComposite` after SFSE has loaded every plugin. Installation is
fail-closed except for Luma's proven `ScaleformComposite` call-through hook:
OSF UI chains that owner after Luma has patched the vanilla implementation.
Any other partial or foreign hook disables UI-pass drawing. Since the present-time
renderer has been retired, menu opens are then refused so an invisible overlay
cannot capture input; the failure is logged at error level by both `UiPass`
and `Runtime`.

Pass execution moves among render workers, so the implementation retains no
engine resource or command list across calls. The transient target is validated
by state, format, dimensions, and direct-list type at each handoff.

The compositor binds its own RTV, descriptor heap, root signature, and
premultiplied PSO, then restores the engine's tracked descriptor heaps. This is
the known-good behavior from before `b8e3643`; root-signature and
pipeline-state interception is intentionally not part of the release path.

The shared WebView texture ring uses the newest produce-fence-complete slot; if
the newest publication is incomplete, the draw reuses the last ready slot rather
than dropping the overlay for one frame.

No swapchain probe or Present hook is installed. The remaining plumbing is split
between existing safe paths:

- `Submit()` adopts newly announced WebView shared rings on the tick thread;
- the UI target descriptor supplies output dimensions;
- handoff shape supplies FG classification; and
- Present-hook liveness monitoring is unnecessary because no Present dependency remains.

## Diagnostics

The UI-pass draw is unconditional; there is no `uiPassDraw` switch and no present-time
fallback to select. A knob whose off position renders no UI is not a useful
compatibility control, and the fail-closed slot check declines every
foreign owner except the explicitly supported Luma composite hook.

Two consequences of dropping the backbuffer draw are worth noting:

- HDR and `_SRGB` backbuffers no longer suppress the overlay. The overlay renders
  through a typed view matching the stock `R8G8B8A8_UNORM` or Luma
  `R16G16B16A16_FLOAT` UI buffer, so the swapchain's own format is not inspected.
- Frame Generation no longer suspends drawing. The hook infers FG directly from
  the transparent COPY_SOURCE handoff and selects that target without inspecting
  the swapchain or its presenting caller.

The `uiPassProbe` characterization diagnostic has been removed now that the
frame graph, FG target selection, and hand-off decode are baked into the UI-pass
draw. Only its load-bearing hooks remain: the `ScaleformBegin`/`End`/
`Composite` slot-7 hooks and the `ResourceBarrier`/`SetDescriptorHeaps`
command-list hooks the draw needs (hand-off match plus engine heap restore).

The OSF RE sandbox UIPass experiment must remain disabled because it owns the
same vtable slots and has no proven call-through contract.

## Acceptance evidence

The final FSR3-FG run showed:

- the first overlay draw on the premultiplied FG UI input;
- all opaque scene candidates skipped afterward;
- stable opaque and translucent gradient regions across real/generated cadence;
- no mouse-repaint dropout;
- no device removal, crash, or present fallback; and
- load-safe, hitch-free behavior from the preceding acceptance sessions.

## Retirement of the present-time renderer

Done. The backbuffer renderer, probe swapchain, slot-8 Present hook, swapchain
discovery table, FG caller classifier, and hook-liveness watchdog have all been
removed. Output sizing and FG target selection come from `UiPass`, while
shared-ring adoption runs from `Submit()` on the tick thread.

The universal UI-pass-only build still requires the full in-game compatibility
matrix: vanilla FG off, native FSR3 FG, OptiScaler/Nukem FG, OptiScaler plus the
Steam overlay, and display-mode or resolution changes.
