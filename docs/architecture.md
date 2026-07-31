# Architecture

## Backends

The production path is `WebView2HostWebRenderer` + `D3D12Compositor`. Null backends remain selectable from config for development and fault isolation:

- `NullCompositor` receives frames and logs them instead of drawing;
- `NullWebRenderer` is the fallback when a configured backend can't initialize (missing SDK/runtime files); initialization failures are logged, not fatal.

Backends implement `IWebRenderer` / `ICompositor`; the rest of the runtime doesn't depend on which one is active.

The shipped `data/OSFUI/config.json` no longer authors the `renderer` / `compositor` keys (or the other development switches) — `core/Config` still parses them and everything unlisted falls back to its built-in default, so selecting a stand-in backend means adding the key by hand.

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
                                ┌────┴─────┐  ┌──┴─────────┐    (WndProc subclass)
                                │ Null     │  │ Null       │   HardwareCursor
                                │ WebView2 │  │ D3D12      │   UiLayoutGuard
                                └──────────┘  └────────────┘   MenuEventSink
                                                               FocusMenu / ControlLayer
                                   │    ▲
                          runtime/MessageBridge    JSON, whitelisted commands
                                   │
                    ┌──────────────┼──────────────┐
                 api/          api/            reporting/
                 BridgeApi     PapyrusApi      ReportClient
                 (C ABI for    (OSFUI.psc      (consented, bounded
                  SFSE mods)    natives)        log upload)
```

Two subsystems hang off the bridge rather than the render path:

- `api/` is the public extension surface — `BridgeApi` backs the exported
  `OSFUI_RequestBridge` C ABI ([native-plugin-api.md](native-plugin-api.md)),
  `PapyrusApi` backs the shipped `OSFUI` script's natives
  ([authoring-dynamic-data.md](authoring-dynamic-data.md)). Both marshal onto
  the main thread and derive a caller's mod id from the trusted source rather
  than the payload.
- `reporting/` is the bug reporter: it is the one native egress path in the
  process, is callable only by the built-in `osfui/settings` view, and never
  sends without explicit consent (see [security-model.md](security-model.md)
  rule 5).

### Data flow per frame

1. An SFSE permanent task (registered in `core/Plugin.cpp`) runs on the engine's render-graph workers and posts one coalesced `Runtime::Tick(dt)` through `RE::BSService::TaskQueue`. The queue drains Tick on the game main thread; if BSService cannot enqueue yet, the worker drops that notification and retries next frame rather than taking the queue's unsafe inline fallback. Tick self-times on the main thread and clamps `dt` to 100 ms.
2. `IWebRenderer::Update(dt)` advances the web content.
3. The WebView2 host publishes frames through a shared D3D12 texture ring; `IWebRenderer::Render()` returns the ready slot and fence serial.
4. `ICompositor::Submit(frame)` records that slot; the overlay is drawn later, inside the engine's Scaleform UI pass (see *How the D3D12 compositor works*), sampling the shared texture directly with no CPU readback or upload.

### Message bridge

All native↔web traffic is JSON text with shape `{ "type": string, "payload": object }` through `MessageBridge`. The bridge has no built-in knowledge of any feature: it transports messages and dispatches `ui.command` through a handler registry.

- web → native: only `ui.command`, whose `command` is looked up in the registry (`MessageBridge::RegisterCommand`). Unknown commands are rejected and logged.
- native → web: `MessageBridge::SendToWeb(type, payload)`; `runtime.ready` / `runtime.pong` are the platform messages. Delivered via `IWebRenderer::SendMessageToWeb`.

The bridge is constructed with just a `SendFn` transport (wired to the renderer). Commands are registered by:
- core — platform/window commands only (`close`, `setVisible`, `log`, `ping`), via `Runtime::RegisterPlatformCommands`;
- each feature module — its own namespace (e.g. settings registers `settings.get`/`set`/`reset`).

### Feature modules ("apps" on the platform)

Features are `IUiModule`s (`runtime/UiModule.h`). `IUiModule` is a uniform lifecycle fan-out: the runtime drives every module through the same points — `OnStart()` (applies persisted state at load), `RegisterCommands(bridge)` (wire its own bridge commands), `OnBridgeDown()`, `OnViewDestroyed()` — from one loop in registration order, rather than a per-module call at each site. It is not a decoupling seam: the runtime still owns and reaches through the concrete module types directly.
`Runtime::BuildModules()` is the composition root - the one place that names concrete modules and injects their dependencies

### Views

`ViewManager` does a **two-level** scan of `<data>/views/<modId>/<viewName>/manifest.json`. The first level is a mod namespace (its folder name must pass the mod-id grammar; `shared/` is skipped as the asset kit, and a manifest found at the first level is rejected as the pre-1.0 flat layout). The second level is the view. **The path is the identity**: the qualified view id is `<modId>/<viewName>`, derived from the folder, never from the file — the manifest's own `id` is only checked for consistency against the view folder name, so a manifest cannot claim another mod's namespace. Subfolders without a `manifest.json` are ignored, so a mod can keep shared assets beside its views.

A `ViewManifest` declares id, entry page, size, transparency, and a permission block that defaults to deny (`nativeBridge`, `filesystem`, `network`). Manifest entries may not point outside the view folder.

Discovery does not create browser content. The runtime creates a view on its
first open, except for `openOnStart` startup candidates and the warm core set
(the handoff surface plus `config.warmViews`). A hidden live view becomes
eligible for best-effort WebView2 suspension after 90 seconds of clamped game
time. Non-warm views are destroyed after 25 hidden minutes and return to the
discovered state; warm views may suspend but are never idle-reclaimed.

### Frontend build

The built-in views are generated, not hand-written. `frontend/` is a Vite + TypeScript + Preact project whose ignored build artifact is `build/frontend/views/`:

```
frontend/src/  ──(npm run build)──►  build/frontend/views/  ──► xmake install / MO2 redeploy / package.ps1
```

- Per view the build emits `main.js` and `style.css`, and copies `index.html` + `manifest.json` through unprocessed — Vite's HTML pipeline would inject `type="module"` and `crossorigin` and hash asset names, all three of which break the shipped contract.
- `views/shared/osfui.{js,css}` and `views/osfui/padnav.js` are copied **verbatim** from source and asserted byte-identical on every build; they are compatibility boundaries, not unfinished work (`frontend/COMPATIBILITY.md`).
- Output filenames are stable — no content hashes — so public asset paths and installed layouts remain deterministic.
- The generated tree is ignored. `xmake build` and `xmake install` run the frontend builder before consuming it, while release packaging installs the locked npm dependencies first. Node is a developer/build dependency, never a player runtime dependency.

Nothing in the native runtime is frontend-aware: it discovers whatever manifests are on disk, exactly as it does for a third-party mod's hand-authored view.

## How the WebView2 backend fits

`WebView2HostWebRenderer` launches `osfui_webview2_host.exe` outside the
game's process tree, communicates over a framed named pipe, and receives
browser frames through shared D3D12 textures. Keeping Chromium out of process
avoids MO2/USVFS injection into `msedgewebview2.exe`. The game creates the
owner-only server pipe before launch, and both ends compare the kernel-reported
peer PID with the expected process before accepting the session. Starfield-side
writes run only on a bounded transport worker; the reader applies a total hello
deadline and a heartbeat deadline so an alive but stalled helper is recoverable.

The overlay host owns one WebView2 environment and one composition controller
for each live view. These remain separate browsing instances for fault and DOM
isolation; Chromium decides their renderer-process assignment. Because lazy or
idle-reclaimed views have no controller, browser resource use generally tracks
views used in the current session rather than every installed manifest.

## How the D3D12 compositor works

`D3D12Compositor` implements `ICompositor` on the game's own D3D12 device. Frames are sampled directly from the WebView2 host's shared texture ring with no CPU readback or upload. The overlay quad is drawn *inside the engine's Scaleform UI pass* (`composite/UiPassSeam`, hooked at ScaleformEnd) so frame generation (FSR3 / DLSS-G) paces the overlay like native UI. The compositor does not hook `IDXGISwapChain::Present`: Submit adopts shared rings on the tick thread, while the seam reports output dimensions and identifies the transparent `COPY_SOURCE` UI hand-off used when frame generation is active. This keeps OSF UI outside Present chains owned by OptiScaler, Streamline, Steam, RTSS, ReShade, and similar tools.

Remaining open areas: alternate UI-target formats and broader in-game validation across frame-generation and external-overlay combinations.

## Lifetime

- `SFSE_PLUGIN_LOAD` → `Runtime::Get().Initialize()`: paths → config → views → renderer → compositor → bridge → input config.
- SFSE has no shutdown callback, so engine-facing singletons intentionally have process lifetime and do not run destructors from DLL detach.
- Normal in-session recovery and explicit renderer stops use a synchronized lifecycle: stop new sends, give the background writer 250 ms to deliver shutdown, cancel all overlapped pipe I/O, join transport workers, then wait a bounded interval for the verified helper and terminate it only if necessary.
- During process exit the helper independently watches the Starfield process handle; correctness never depends on destructor ordering after Windows begins process teardown.
