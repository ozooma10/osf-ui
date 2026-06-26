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
- ✅ JS bridge: `window.osfui.postMessage` injected at
  `OnWindowObjectReady` via the JSC C API; native→web calls
  `window.osfui.onMessage(json)`; messages queue until DOM ready.
- ✅ Cadence: a dedicated worker thread owns ALL Ultralight state and
  self-paces at ~60 Hz; the game thread only touches queues and a
  double-buffered frame copy. (Required: WebKit is thread-affine, SFSE ticks
  arrive on varying threads, and heavyweight init in SFSE's pre-main load
  phase deadlocks — the worker starts lazily on the first tick.)
- ✅ Exit criteria met: post-DOM frame PNG-dumped from in-game
  (`<data>/ultralight/first-frame.png`, shows the rendered test page with
  "Connected: OSF UI v0.1.0"), and the bridge round-trips proven by
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

Phase 3 polish:
- ✅ **Aspect/scaling (2026-06-13).** The view is sized to the real output
  surface instead of a fixed 1280×720: the compositor reports the backbuffer
  size (`ICompositor::SetOutputResizeCallback`), Runtime resizes the
  Ultralight view to match the screen aspect (height-capped at 1440 so CPU
  raster stays bounded; the page is responsive), and the draw fills the
  backbuffer with equal aspect → no distortion. Cursor sensitivity rescales
  with view width so it feels the same at any resolution. Verified in-game:
  crisp + undistorted at 1920×1080; the earlier ultrawide stretch is gone.
- ✅ **Resize crash (2026-06-13).** No longer caches backbuffer refs (was
  blocking `ResizeBuffers`); `GetBuffer` per-present.
- ⏳ HDR backbuffers (R16G16B16A16_FLOAT) and the 10-bit path untested — runs
  so far were SDR 8-bit (format 28). Multi-format PSO when a swapchain
  differs (currently one PSO; a second format is skipped).
- ⏳ Two-swapchain / frame-gen: we draw on both; pick the scanned-out one
  when DLSS-G/Streamline is active (this dev GPU is Ampere, no frame-gen).
- ⏳ Coexistence with Steam overlay / ReShade / RTSS (hook-chain ordering);
  alt-tab / exclusive-fullscreen transitions.
- ⏳ sRGB/color-management correctness pass; per-present upload of only the
  dirty region (big views re-upload the whole frame on each repaint).

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

### 5a — schema-driven settings ✅ (verified in-game 2026-06-13)

- ✅ A mod ships `settings/schema.json` (groups of typed settings:
  bool/int/float/enum/string with default/min/max/step/options). `SettingsStore`
  loads it read-only and merges persisted values over the defaults.
- ✅ Built-in `settings` view renders the schema into real controls
  (checkbox/slider/dropdown/text) with zero per-mod native code; each change
  sends `ui.command settings.set` through the bridge.
- ✅ `SettingsStore::Set` validates + clamps every value against the schema
  (unknown keys rejected, numbers clamped to min/max, enum must be an option,
  strings length-bounded) and persists atomically (temp file + rename) to a
  USER-WRITABLE path (`Documents\My Games\Starfield\OSFUI\settings.json`,
  via `SHGetKnownFolderPath` — NOT the read-only/MO2-mapped data dir).
- ✅ Bridge gains exactly two whitelisted commands: `settings.get`
  (native → `settings.data`) and `settings.set` (→ `settings.ack`). Still no
  arbitrary native calls.
- ✅ Verified in-game: form renders from the schema; changing controls
  persists (opacity clamped to the schema min server-side); values survive a
  relaunch (load → merge → render).

### 5b — registry + reactions ✅ (verified in-game 2026-06-13)

- ✅ **Multi-mod registry:** `SettingsStore` loads every `settings/*.json` as
  a separate mod (id from the schema or filename), each persisting to its own
  `<docs>/OSFUI/settings/<id>.json`. `DataJson` returns
  `{ mods: [...] }`; the settings view renders a card per mod. Verified with
  two mods (`osfui` + `demo`).
- ✅ **Reset-to-default:** `settings.reset` command (per-key or whole mod);
  each mod card has a Reset button. Re-sends the registry so the view
  re-renders. Verified: demo values snapped back to defaults.
- ✅ **Change notifications → native reactions (the payoff):**
  `SettingsStore` fires a listener on every committed value (Set/Reset and
  once per value via `NotifyAll` at startup). `Runtime::OnSettingChanged`
  reacts — `osfui.cursorSpeed` live-scales mouse sensitivity. Verified
  in-game: dragging the slider changes the cursor's pace immediately. (Chose
  cursor speed over an input-capture toggle, which would have been a UX trap:
  a non-capturing overlay can't be clicked to re-enable itself.)

### 5c — later

- Multi-view management + view switching; per-view permissions enforced in the
  bridge; versioned bridge API for third-party views; settings grouped/tabbed
  by mod; richer types (color, key-bind, list); change notifications to other
  native plugins.

### Multi-view feasibility (spike, 2026-06-13)

Desk-checked against the vendored Ultralight 1.4.0 headers (no game needed) to
de-risk multi-view before committing to it. Verdict: **supported and intended;
a bounded engineering effort, not blocked by the SDK.**

- One `Renderer` hosts N `View`s by design (`Renderer::CreateView` is a
  factory; `Render()` renders "all active views" — `include/Ultralight/Renderer.h`
  lines 100, 171, 207). The one hard limit — "only create one Renderer during
  the lifetime of your program" (Renderer.h:140) — is already honored, so
  multi-view = N views on the single renderer we already create.
- **Idle views are cheap:** "Views are only repainted if they actually need
  painting" (Renderer.h:209); `RenderOnly(view_array, len)` targets a subset. A
  static HUD costs ~0 raster after first paint. Cost scales with *active
  repaints*, not view count: N WebKit pages (memory) + N CPU bitmaps.
- Each View owns its surface/texture (Renderer.h:114), so the compositor draws
  N textures back-to-front (z-order from the manifest); the SRV heap grows from
  1 to N descriptors. Per-View sessions are supported (Renderer.h:149).
- **The trickiest internal change is NOT rendering — it's bridge message
  attribution.** The JSC `postMessage` callback is a plain C function pointer
  with no user-data slot and finds the renderer via a single `sActive`
  (`src/render/UltralightWebRenderer.cpp` ~line 261). With N views it must
  identify the *source* view — key by `JSContextRef`, or inject a per-view
  bridge object carrying the view id. Input focus (one view receives
  keys/mouse) is the other routing change.
- Correction to an earlier note: the remote inspector is **Pro-edition only**
  (Renderer.h:239), so "wire the inspector in devMode" needs a Pro license.

Recommended M2.1 shape: `ViewManager` tracks active views; the renderer holds a
view list keyed by id (per-view input/message queues + frame buffers); the
compositor draws a z-ordered list of textures; a focus model picks the input
target. Per-view bridge permission scoping becomes meaningful once >1
third-party view can be active and should land together with this.
