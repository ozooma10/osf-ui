# Architecture

## Goal

PrismaUI SF is a prototype runtime for hosting HTML/CSS/JS-based UI inside
Starfield, delivered as an SFSE/CommonLibSF plugin. The concept is inspired by
Prisma UI (Skyrim); no Prisma source code is used.

## Why the first milestone uses mock/null backends

Everything that would make this *visibly* work in-game — rendering HTML,
drawing over the game's image, capturing input — depends on reverse-engineered
integration points (render hook, input source, update cadence) that have **not**
been established yet, and this project refuses to guess addresses or fake
hooks. So milestone 0 builds the entire pipeline shape with honest stand-ins:

- the **MockWebRenderer** produces a real CPU RGBA buffer (an animated test
  pattern) so the renderer → compositor data path is exercised end-to-end;
- the **NullCompositor** receives those frames and logs them instead of
  drawing;
- the **D3D12Compositor** stub *fails initialization on purpose* so nothing
  can mistake it for a working presentation path.

When the real integration points are proven (see
[reverse-engineering-notes.md](reverse-engineering-notes.md)), backends are
swapped behind stable interfaces without touching the rest of the runtime.

## Layers

```
                 SFSE_PLUGIN_PRELOAD / SFSE_PLUGIN_LOAD   (src/main.cpp)
                                   │
                            core/Plugin.cpp        entry glue, SFSE messages
                                   │
                          runtime/Runtime          owns everything below
        ┌───────────────┬──────────┼──────────────┬──────────────────┐
        │               │          │              │                  │
   core/Config    runtime/      render/       composite/         input/
   core/Paths     ViewManager   IWebRenderer  ICompositor        InputRouter
                  ViewManifest     │              │                  ▲
                                ┌──┴────────┐  ┌──┴─────────┐  UiInputHook
                                │ Null      │  │ Null       │  (observe-only
                                │ Mock      │  │ D3D12 stub │   vfunc, gated)
                                │ Ultralight│  └────────────┘  MenuEventSink
                                │  (option) │                  (RegisterSink)
                                └───────────┘
                                   │    ▲
                          runtime/MessageBridge    JSON, whitelisted commands
```

### Data flow per frame

1. An SFSE permanent task (registered in `core/Plugin.cpp`) calls
   `Runtime::Tick(dt)` every frame on the game's Main thread, with a
   self-timed, 100 ms-clamped dt. See RE notes §1 for the evidence and
   constraints (process-lifetime delegate, runs under SFSE's queue lock).
2. `IWebRenderer::Update(dt)` advances the web content.
3. If the overlay is visible, `IWebRenderer::Render()` returns a
   `FrameBufferView` — a *non-owning* view of CPU pixels valid only until the
   next renderer call.
4. `ICompositor::Submit(frame)` consumes it immediately (copy/upload), never
   stores it.

### Message bridge

All native↔web traffic is JSON text with shape
`{ "type": string, "payload": object }` through `MessageBridge`. The bridge is
**feature-agnostic**: it transports messages and dispatches `ui.command` by a
registry of handlers — it has no built-in knowledge of any feature.

- web → native: only `ui.command`, whose `command` is looked up in a handler
  registry (`MessageBridge::RegisterCommand`). Unknown commands are rejected
  and logged. There is no generic "call native" escape hatch.
- native → web: `MessageBridge::SendToWeb(type, payload)`; `runtime.ready` /
  `runtime.pong` are the platform messages. Delivered via
  `IWebRenderer::SendMessageToWeb`.

The bridge is constructed with just a `SendFn` transport (wired to the
renderer). Commands are registered by:
- **core** — platform/window commands only (`close`, `setVisible`, `log`,
  `ping`), via `Runtime::RegisterPlatformCommands`;
- **each feature module** — its own namespace (e.g. settings registers
  `settings.get`/`set`/`reset`).

### Feature modules ("apps" on the platform)

Features are `IUiModule`s (`runtime/UiModule.h`). The core runtime hosts them
without knowing what they do: `OnStart()` applies persisted state at load and
`RegisterCommands(bridge)` lets a module wire its own bridge commands.
`Runtime::BuildModules()` is the composition root — the one place that names
concrete modules and injects their dependencies (e.g. the settings module gets
a change-listener so core can react to the knobs it owns, like cursor speed).

Today there is one module, `SettingsModule` (schema-driven settings, Phase 5),
but the seam is deliberate: a HUD, quest tracker, etc. would each be another
module, and a future public plugin API would expose `RegisterCommand` +
view-registration so modules could ship in separate DLLs (the SKSE/SkyUI
split). The settings *engine* stays native because validation/persistence must
not be trusted to untrusted JS (see security-model.md).

### Views

`ViewManager` scans `<data>/views/*/manifest.json`. A `ViewManifest` declares
id, entry page, size, transparency, and a permission block that defaults to
deny (`nativeBridge`, `filesystem`, `network`). Manifest entries may not point
outside the view folder.

## How an Ultralight backend fits

`UltralightWebRenderer` (compiled only with the `with_ultralight` xmake option
+ `ULTRALIGHT_SDK_DIR`) implements `IWebRenderer` over an offscreen Ultralight
renderer using the CPU `BitmapSurface`. It slots in where the mock renderer
sits today: same `LoadView`, same `Render() → FrameBufferView`, same JSON
bridge via a single injected `window.prisma.postMessage` function. The rest
of the runtime does not know which backend is active. The SDK is proprietary
and never vendored; the default build has zero Ultralight footprint.

## How a D3D12 compositor fits

The eventual `D3D12Compositor` implements `ICompositor`: on `Submit` it
uploads the CPU frame into a texture (staged ring buffer) and records an
alpha-blended fullscreen-quad draw into the game's frame at present time. All
of its open questions (device/queue access, present timing, descriptor heaps,
state transitions, HDR/scaling, overlay coexistence) are listed in
`composite/D3D12Compositor.h` and in the RE notes; until they are answered the
stub refuses to initialize.

## Lifetime

- `SFSE_PLUGIN_LOAD` → `Runtime::Get().Initialize()`: paths → config → views →
  renderer → compositor → bridge → input config.
- SFSE has **no shutdown callback**; `Runtime::Shutdown()` exists but is
  currently unreachable. All state must tolerate being torn down by process
  exit instead.
