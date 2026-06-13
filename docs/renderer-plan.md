# Renderer Plan

Phased plan from "compiles honestly" to "real web UI in-game". Each phase is
shippable on its own; no phase fakes the next one.

## Phase 0 — buildable plugin skeleton ✅ (current)

- SFSE/CommonLibSF plugin builds with `xmake build`, no Ultralight installed.
- Lifecycle logging (preload/load/SFSE messages).
- Config + view manifest loading from the plugin data folder.
- `IWebRenderer` / `ICompositor` interfaces with Null and Mock backends.
- Mock backend produces an animated CPU RGBA test pattern; Null compositor
  logs submissions.
- JSON message bridge with a whitelisted command set.
- Per-frame `Runtime::Tick()` driven by `SFSE::TaskInterface` permanent tasks
  (main thread, SFSE-maintained hook — see reverse-engineering-notes.md §1).
  In-game cadence verification (main menu / pause / loading) still pending.

## Phase 1 — local HTML renderer offscreen ✅ (verified in-game 2026-06-12)

- ✅ `UltralightWebRenderer` implemented behind `with_ultralight` (Ultralight
  1.4.0 SDK, CPU `BitmapSurface`, in-memory session).
- ✅ `SandboxFileSystem` restricted to the active view's folder + the ICU
  resources dir; AppCore's DirectWrite font loader (only AppCore use); no
  clipboard handler; network: see the documented gap in security-model.md §2.
- ✅ JS bridge: `window.starfield.postMessage` injected at
  `OnWindowObjectReady` via the JSC C API; native→web calls
  `window.starfield.onMessage(json)`; messages queue until DOM ready.
- ✅ Cadence: a dedicated worker thread owns ALL Ultralight state and
  self-paces at ~60 Hz; the game thread only touches queues and a
  double-buffered frame copy. (Required: WebKit is thread-affine, SFSE ticks
  arrive on varying threads, and heavyweight init in SFSE's pre-main load
  phase deadlocks — the worker starts lazily on the first tick.)
- ✅ Exit criteria met: post-DOM frame PNG-dumped from in-game
  (`<data>/ultralight/first-frame.png`, shows the rendered test page with
  "Connected: StarfieldWebUI v0.1.0"), and the bridge round-trips proven by
  the test view's automatic `log` + `ping` handshake (no click needed —
  input routing is Phase 4).

## Phase 2 — CPU bitmap upload to D3D12 texture ✅ (verified in-game 2026-06-12)

- ✅ Device/queue route proven first (TODO #4, in `OSF RE/` — see
  reverse-engineering-notes.md §2); `composite/EngineD3D12.cpp` re-verifies
  it at runtime with QI + DIRECT-type + COM-identity checks before use.
- ✅ `D3D12Compositor`: 3-slot upload ring (persistent-mapped staging
  buffers, 256-aligned row pitch, per-slot fence values); rows copied
  honoring `strideBytes`; BGRA8/RGBA8 both map to matching DXGI formats;
  busy ring skips frames (never blocks the game thread); identical frames
  deduped by `frameIndex`. Engine objects located lazily on first Submit
  (the renderer root global is empty at SFSE-load time).
- ✅ Exit criterion met with an automated, stronger substitute for the PIX
  capture: a one-shot devMode GPU round-trip (texture -> readback buffer ->
  byte-compare vs the submitted pixels) logged
  `ROUND-TRIP VERIFIED — GPU texture matches the submitted frame`
  (2026-06-12 21:37 run). The game kept rendering normally with uploads
  interleaving on its direct queue. A manual PIX/RenderDoc capture remains
  worthwhile before Phase 3 draw work.

## Phase 3 — in-game overlay composition ✅ (first light verified in-game 2026-06-12)

- ✅ Present-time draw of an alpha-blended fullscreen quad over the game
  image, **visually confirmed on screen** (user-verified), stable across
  thousands of presents with no GPU fault.
- ✅ Hook mechanism: `IDXGISwapChain::Present` vtable slot 8 (TODO #5
  decision), captured via a throwaway swapchain — pure DXGI, zero engine
  offsets. The vtable is shared process-wide so one swap covers every
  swapchain.
- ✅ All overlay GPU work runs on the render thread inside the Present hook
  (upload cached CPU frame → texture, then draw); `Submit` on the tick
  thread only caches pixels. No cross-thread resource-state coordination.
- ✅ Own root signature, premultiplied-alpha PSO (built for the live RT
  format), SRV/RTV heaps, fence-guarded command-allocator ring, per-slot
  upload buffers. Backbuffer PRESENT→RENDER_TARGET→PRESENT, texture
  COPY_DEST↔PIXEL_SHADER_RESOURCE each frame.
- Verified config: 1920×1080 R8G8B8A8_UNORM backbuffer, two swapchains
  (drew on both), 1280×720 overlay scaled to fill.

Phase 3 polish still open (none block "pixels on screen"):
- Aspect/scaling: the overlay currently stretches to fill; add native-size
  or aspect-correct placement for real views.
- HDR backbuffers (R16G16B16A16_FLOAT) and the 10-bit path untested — this
  run was SDR 8-bit. Multi-format PSO when a swapchain differs.
- Two-swapchain / frame-gen: we draw on both; pick the scanned-out one when
  DLSS-G/Streamline is active (this dev GPU is Ampere, no frame-gen).
- Coexistence with Steam overlay / ReShade / RTSS (hook-chain ordering);
  resize / alt-tab / exclusive-fullscreen transitions.
- sRGB/color-management correctness pass.

## Phase 4 — input focus and text entry

### 4a — focus + consumption + keyboard ✅ (verified in-game 2026-06-12)

- ✅ Focus model: `captureInput` config + overlay-visible = captured.
  Runtime::IsInputCaptured() is the single source of truth; F10 toggles, Esc
  closes.
- ✅ **Input consumption — the hard part (TODO #7).** The engine UI input
  sink (UI::PerformInputProcessing) CANNOT block gameplay: movement and
  camera/mouse-look read the same input via sibling sinks, so neither an
  empty UI queue nor marking events consumed (status/disabled) stops the
  player (both proven to fail in-game). The working mechanism is a **WndProc
  subclass on the game window** (`input/OverlayInputHook`, via
  `SetWindowLongPtr` — not a global Win32 hook): while captured it consumes
  `WM_INPUT` (raw mouse-look + keyboard), `WM_KEY*`, `WM_CHAR`, and all mouse
  messages, so the game freezes. Verified in-game: with the overlay open the
  character and camera are fully frozen, F10 releases.
- ✅ Keyboard routed into the view: the WndProc feeds VK codes →
  `InputRouter` → `UltralightWebRenderer::InjectKeyEvent` → `View::FireKeyEvent`
  (RawKeyDown/KeyUp + a Char event for printable keys via
  GetKeyFromVirtualKeyCode). Typing appears in the page's focused field.
  The old engine-event input path (UiInputHook) is now observe-only.

### 4b — mouse + cursor ✅ (verified in-game 2026-06-12)

- ✅ `WM_INPUT` raw mouse deltas parsed in the WndProc → a virtual cursor in
  view space (Runtime owns it; the OS cursor is hidden in gameplay so we
  accumulate deltas, clamp to the view) → routed as MouseMoved/Down/Up into
  the view (`UltralightWebRenderer::InjectMouse*` → `View::FireMouseEvent`).
  Raw button flags (RI_MOUSE_*) drive clicks; legacy mouse messages are
  consumed as duplicates.
- ✅ Visible software pointer drawn by the page from routed mousemoves
  (CSS arrow; native cursor hidden).
- ✅ **Full interaction loop verified in-game**: the cursor moves, Ping/Close
  highlight on hover, clicking Ping round-trips web→native→web (`runtime.pong`
  in the log + "Pong received" status), clicking Close hides the overlay.

### 4c — remaining input polish (later)

- Scroll wheel (RI_MOUSE_WHEEL → ScrollEvent); IME/Unicode text (WM_CHAR
  path; current VK→char is US-layout only); gamepad + controller parity;
  cursor sensitivity setting; aspect-correct cursor mapping for non-16:9.

## Phase 5 — MCM-style schema-driven UI

- A settings schema (JSON) that mods can ship; the runtime renders it with a
  built-in view and persists values.
- Multi-view management, per-view permissions enforced in the bridge,
  versioned bridge API for third-party views.
