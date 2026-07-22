# Seam draw: overlay inside the engine's UI render pass

Status: DESIGN v2 (2026-07-21). v1's injection point (composite pass) was
corrected same-day after the frame-graph structure landed in OSF RE
`rendering.ui_pass` + the first OSF UI capture session. Probe groundwork is in
`src/composite/UiPassSeam.cpp` (`uiPassProbe` dev knob); this doc specifies the
actual draw. Not scheduled for a release until proven in-game behind a dev knob.

## Why

The present-time slot-8 overlay draw is fundamentally incompatible with Frame
Generation: once FG (DLSS-FG or the exe's built-in FSR3) owns the present chain,
one foreign draw into ANY swapchain of the chain crashes the game (proven
2026-07-21 — see CHANGELOG Unreleased and the FrameGenActive() suspension).
Both FG vendors composite the game's UI from a pre-present UI buffer onto real
AND generated frames. Anything drawn into that buffer therefore rides through
FG correctly, for both vendors, with no vendor-specific code — and the same
seam is the path to overlay-under-native-menus and to inheriting the game's
own output transform (HDR).

## The frame graph (1.16.244, OSF RE `rendering.ui_pass`, runtime-proven)

RootGraphSetup frame tail:
scene+post → `ImageCapture_EndOfFrame_NoUI` (FG only) →
**[4] UI subgraph renders all movies into offscreen `ScaleformCompositeBuffer`** →
**[5] Frame Interpolation node (0x23C)** →
**[6] ScaleformCompositeRenderPass (0x1A8) blends the buffer over the scene** →
end-of-frame capture → present.

UI subgraph (node 0x23B) children, in order: ScaleformBegin (0x1A4) →
DLSSFrameGenerationUIRenderPass (0x189, FG only) → ScaleformToTexture (0x1A7) →
ScaleformRenderPass (0x1A6, one instance per live movie) → ScaleformEnd (0x1A5)
→ ScaleformTextRenderPass (0x9) → ScaleformText2DRenderPass (0xA).

**The decisive consequence:** Frame Interpolation consumes the scene and
`ScaleformCompositeBuffer` SEPARATELY, before the composite pass runs. UI on
*generated* frames is composited by FG itself from the buffer; UI on *real*
frames by pass [6]. Therefore:

- Drawing at the composite pass ([6], design v1) lands on REAL frames only →
  overlay flickers at FG 2x. WRONG seam.
- Drawing into **`ScaleformCompositeBuffer` inside the UI subgraph** lands on
  both real and generated frames, and works identically with FG off. RIGHT
  seam. Over-game-UI = draw at/after ScaleformEnd; under-native-menus variant
  = draw at ScaleformBegin (post-original). Same plumbing, one flag.

## Capture session findings (2026-07-21, OSF UI probe, FG off)

- Both pass hooks install fail-closed and verify (NB: mutually exclusive with
  the OSF RE sandbox's UIPass experiment — it owns the same slots; disable one).
- Composite bracket: exactly one RT bound, no DSV, and **rtv0 is identical
  across all brackets** (stable descriptor) while the recording list churns.
- **`cmdCtx+0x60` holds the recording `ID3D12GraphicsCommandList` in every
  bracket** — stable offset; bracket capture (slots 26/46 thunks) also works
  as an offset-free fallback. Both acquisition strategies are viable.
- No resource barriers recorded inside the composite bracket → transitions
  happen in graph glue, so in-pass drawing needs no barriers of its own, but
  the capture never saw the RT resource → **buffer format/size still unknown**
  (statically unextracted too, per the RE module).

## What still must be pinned down (phase 1b, probe extension)

1. **The `ScaleformCompositeBuffer` resource** (→ format, size, and our own
   RTV). Documented resolution path: pass instance `+0xF0` (24-bit index) →
   `g_RenderGraphResourceMgr` (AddrLib ID 837405; per-table `+0x40` index
   array, `+0x3C8` pool) → resource record → scan with the existing
   `ScanObject`/`ClassifyCandidate` machinery, which already logs
   `ID3D12Resource WxH fmt=...` via GetDesc. Run it on the Begin/End/Render
   pass instances' records inside their brackets.
2. **Where the buffer leaves RENDER_TARGET state.** Hook ScaleformEnd
   (Execute ID 145956, vtable ID 497425, same fail-closed pattern) and extend
   the barrier/OMSet capture bracketing to Begin and End. If End's bracket
   shows the buffer's RT→SRV transition, the draw records PRE-original in End;
   if not (text passes after End draw into it too), POST-original in End is
   safe and also covers being over ScaleformText output — decide from capture.
3. Whether the composite pass's `numRTs=1` target differs per FG mode
   (nice-to-know only; we no longer draw there).

## Draw architecture (v2)

### Injection point

`ScaleformEnd::ExecuteRenderPass` thunk (pre- or post-original per 1b.2), i.e.
after every live movie has rendered into `ScaleformCompositeBuffer`, before
Frame Interpolation consumes it. Under-native-UI mode later = same draw at
`ScaleformBegin` post-original.

### Resources

- List: `cmdCtx+0x60` (getter ABI already runtime-confirmed), with the
  bracket-captured list as cross-check; mismatch → skip frame + warn-once.
- RT: our OWN RTV, created once per resolved buffer resource in the
  compositor's existing rtvHeap (no dependence on engine descriptors; the
  buffer resource pointer is re-resolved per bracket and the RTV re-created on
  change).
- Quad state: existing root sig + srvHeap (CPU texture / shared-ring SRVs),
  PSO from the per-format cache keyed on the buffer's actual format (extend
  `IsSupportedRtFormat`/shader encode if it is not 8-bit UNORM — likely the
  buffer is R8G8B8A8; if R10G10B10A2/FP16, that is the HDR story arriving).
- Record: explicit `OMSetRenderTargets(1, ourRtv)` + viewport/scissor =
  buffer size + `DrawInstanced(3)`; premultiplied-over blend MUST also write
  correct alpha (`a = srcA + destA*(1-srcA)`) because the composite pass (and
  FG) blends the buffer over the scene using its alpha.
- No barriers in v1 (buffer is in RT state inside the subgraph; 1b.2 confirms
  the safe side of the transition).

### Transport & synchronization

Unchanged from v1 of this doc: we cannot fence-bracket a queue we don't
control, so v1 samples the newest ring slot fence-free (rare torn frame,
never a crash; do not signal the consume fence from the seam path), v2
records a ring-slot→private-texture `CopyTextureRegion` on the same list when
the produce fence is CPU-complete, then samples the private copy.

### Interplay with the existing paths

- New dev knob `uiPassDraw`. When on: present-hook DRAW disabled (hook stays
  for output-size discovery, ring adoption, liveness); `FrameGenActive()`
  keeps guarding only the present path, so FG on + seam draw = overlay visible
  THROUGH frame generation. That is the acceptance test.
- Frames with no live movies skip Begin/End entirely (composite early-skip
  path) → no overlay that frame; acceptable (menus always have movies live).
- Kill switches: knob off = today's behavior; every hook fails closed
  (foreign slot value → no hook → present path continues). The OSF RE sandbox
  UIPass experiment must stay disabled when the knob is on (same slots).

## Phases

1. ~~Capture session~~ DONE 2026-07-21 (composite bracket; findings above).
1b. **Probe extension**: hook ScaleformEnd + bracket Begin/End with the call
   capture + resolve the buffer via instance+0xF0 → mgr tables → ScanObject.
   Output: buffer format/size/state side. — NEXT
2. **Recording skeleton**: solid-color debug triangle at the chosen End seam
   behind `uiPassDraw`. Success = tint over game UI, no flicker, no crash,
   stable across menus/loading.
3. **Real overlay**: own-RTV + ring sampling (v1 transport), real-format PSO,
   present-path draw off under the knob.
4. **FG acceptance**: FSR3-FG and DLSS-FG on → overlay visible on real AND
   generated frames (no 2x flicker), FG toggles mid-session survive.
5. **v2 transport + polish**: private-copy transport, HDR encode if the buffer
   demands it, under-native-UI variant (Begin seam), default-flip decision.
