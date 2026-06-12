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
- **Known gap:** nothing calls `Runtime::Tick()` — no verified game update
  hook exists yet.

## Phase 1 — local HTML renderer offscreen

- Implement `UltralightWebRenderer` for real behind `with_ultralight`:
  renderer + offscreen `View` with the CPU `BitmapSurface`.
- Custom `FileSystem` restricted to the active view's folder; fonts; null
  clipboard; network stays off.
- Wire the JS bridge: inject `window.starfield.postMessage`, deliver
  native→web by invoking `window.starfield.onMessage`.
- Drive `Update/Render` from a controlled test cadence (possibly a dev-only
  thread that only touches Ultralight state, never game state).
- Exit criteria: the test view's pixels can be dumped to a PNG/BMP on disk
  and the ping/close buttons round-trip through `MessageBridge`.

## Phase 2 — CPU bitmap upload to D3D12 texture

- Requires the first RE results: a proven way to get the game's
  `ID3D12Device` and a queue (see reverse-engineering-notes.md).
- Upload ring (N staging buffers) sized to the view; copy `FrameBufferView`
  rows honoring `strideBytes`; handle RGBA vs BGRA.
- No drawing yet — exit criteria is a PIX/RenderDoc capture showing the
  texture populated without corrupting a frame.

## Phase 3 — in-game overlay composition

- The hard one: present-time draw of an alpha-blended quad over the game
  image. Needs swapchain/present timing, descriptor heap strategy, resource
  state discipline, HDR/scaling handling, and overlay-coexistence testing
  (Steam, ReShade, RTSS).
- Hook mechanism choice (engine present callback vs IDXGISwapChain hook) is
  decided here, not before; isolated and removable.

## Phase 4 — input focus and text entry

- Identify the real Starfield input event source (PC keyboard/mouse first).
- Focus model: overlay-visible captures input vs pass-through; ESC behavior;
  pause interaction.
- Route through `InputRouter` into the web view (Ultralight key/mouse
  events); IME/text input last.

## Phase 5 — MCM-style schema-driven UI

- A settings schema (JSON) that mods can ship; the runtime renders it with a
  built-in view and persists values.
- Multi-view management, per-view permissions enforced in the bridge,
  versioned bridge API for third-party views.
