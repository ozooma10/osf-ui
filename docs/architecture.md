# Architecture

## Backends

The production path is `UltralightWebRenderer` + `D3D12Compositor`. The stand-in backends remain selectable from config for development and fault isolation:

- `MockWebRenderer` produces a CPU RGBA buffer (an animated test pattern), exercising the renderer → compositor path without the proprietary
  Ultralight SDK;
- `NullCompositor` receives frames and logs them instead of drawing;
- `NullWebRenderer` is the fallback when a configured backend can't initialize (missing SDK/runtime files); initialization failures are logged, not fatal.

Backends implement `IWebRenderer` / `ICompositor`; the rest of the runtime doesn't depend on which one is active.

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
                  ViewManifest     │              │              OverlayInputHook
                                ┌──┴────────┐  ┌──┴─────────┐    (WndProc subclass)
                                │ Null      │  │ Null       │   HardwareCursor
                                │ Mock      │  │ D3D12      │   UiLayoutGuard
                                │ Ultralight│  └────────────┘   MenuEventSink
                                │  (option) │                   FocusMenu
                                └───────────┘                   ControlLayer
                                   │    ▲
                          runtime/MessageBridge    JSON, whitelisted commands
```

### Data flow per frame

1. An SFSE permanent task (registered in `core/Plugin.cpp`) calls `Runtime::Tick(dt)` every frame on the game's Main thread, with a self-timed, 100 ms-clamped dt.
2. `IWebRenderer::Update(dt)` advances the web content.
3. If the overlay is visible, `IWebRenderer::Render()` returns a `FrameBufferView`, a non-owning view of CPU pixels valid only until the next renderer call.
4. `ICompositor::Submit(frame)` consumes it immediately (copy/upload), never
   stores it.

### Message bridge

All native↔web traffic is JSON text with shape `{ "type": string, "payload": object }` through `MessageBridge`. The bridge has no built-in knowledge of any feature: it transports messages and dispatches `ui.command` through a handler registry.

- web → native: only `ui.command`, whose `command` is looked up in the registry (`MessageBridge::RegisterCommand`). Unknown commands are rejected and logged.
- native → web: `MessageBridge::SendToWeb(type, payload)`; `runtime.ready` / `runtime.pong` are the platform messages. Delivered via `IWebRenderer::SendMessageToWeb`.

The bridge is constructed with just a `SendFn` transport (wired to the renderer). Commands are registered by:
- core — platform/window commands only (`close`, `setVisible`, `log`, `ping`), via `Runtime::RegisterPlatformCommands`;
- each feature module — its own namespace (e.g. settings registers `settings.get`/`set`/`reset`).

### Feature modules ("apps" on the platform)

Features are `IUiModule`s (`runtime/UiModule.h`). The core runtime hosts them without knowing what they do: `OnStart()` applies persisted state at load and `RegisterCommands(bridge)` lets a module wire its own bridge commands.
`Runtime::BuildModules()` is the composition root - the one place that names concrete modules and injects their dependencies

### Views

`ViewManager` does a **two-level** scan of `<data>/views/<modId>/<viewName>/manifest.json`. The first level is a mod namespace (its folder name must pass the mod-id grammar; `shared/` is skipped as the asset kit, and a manifest found at the first level is rejected as the pre-1.0 flat layout). The second level is the view. **The path is the identity**: the qualified view id is `<modId>/<viewName>`, derived from the folder, never from the file — the manifest's own `id` is only checked for consistency against the view folder name, so a manifest cannot claim another mod's namespace. Subfolders without a `manifest.json` are ignored, so a mod can keep shared assets beside its views.

A `ViewManifest` declares id, entry page, size, transparency, and a permission block that defaults to deny (`nativeBridge`, `filesystem`, `network`). Manifest entries may not point outside the view folder.

### Frontend build

The built-in views are generated, not hand-written. `frontend/` is a Vite + TypeScript + Preact project whose build output *is* `data/OSFUI/views/`:

```
frontend/src/  ──(npm run build)──►  data/OSFUI/views/  ──► xmake install / MO2 redeploy / package.ps1
```

- Per view the build emits `main.js` and `style.css`, and copies `index.html` + `manifest.json` through unprocessed — Vite's HTML pipeline would inject `type="module"` and `crossorigin` and hash asset names, all three of which break the shipped contract.
- `views/shared/osfui.{js,css}` and `views/osfui/padnav.js` are copied **verbatim** from source and asserted byte-identical on every build; they are compatibility boundaries, not unfinished work (`frontend/COMPATIBILITY.md`).
- Output filenames are stable — no content hashes. The MO2 after-build redeploy overlays without pruning, so hashed names would accumulate orphans, and the staleness gate byte-compares.
- The generated tree is **committed**. The three consumers above read `data/` directly and none of them can run Node, so Node is a frontend-build dependency only — not a runtime one, and not required by `xmake build`, the native tests, or `xmake install`. CI rebuilds and byte-compares (`npm run check:dist`) to keep the committed output honest.

Nothing in the native runtime is frontend-aware: it discovers whatever manifests are on disk, exactly as it does for a third-party mod's hand-authored view.

## How an Ultralight backend fits

`UltralightWebRenderer` implements `IWebRenderer` over an offscreen Ultralight renderer using the CPU `BitmapSurface`.

## How the D3D12 compositor works

`D3D12Compositor` implements `ICompositor` on the game's own D3D12 device: on `Submit` it uploads the CPU frame into a GPU texture, and an `IDXGISwapChain::Present` slot-8 vtable hook draws an alpha-blended fullscreen quad over the backbuffer before the original Present runs. 

Remaining open areas: HDR/10-bit backbuffers, frame-gen swapchain selection, and coexistence with other overlay hooks

## Lifetime

- `SFSE_PLUGIN_LOAD` → `Runtime::Get().Initialize()`: paths → config → views → renderer → compositor → bridge → input config.
- SFSE has no shutdown callback; `Runtime::Shutdown()` exists but is currently unreachable. All state must tolerate being torn down by process exit instead.
