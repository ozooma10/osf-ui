# Architecture

The component, version, view, and lifecycle names used here are defined in the
[terminology glossary](terminology.md).

## Runtime components

The production render path is fixed:

- `WebView2HostWebRenderer` is the game-side web renderer. It launches and
  communicates with the out-of-process browser host,
  `osfui_webview2_host.exe`.
- `D3D12Compositor` draws the browser host's shared textures in Starfield's UI
  pass.

Initialization failure disables OSF UI runtime setup instead of leaving a loaded but
invisible plugin. The runtime owns the fixed `WebView2HostWebRenderer` and
`D3D12Compositor` directly; configuration does not select null or alternate
production implementations.

## Layers

```
                 SFSE_PLUGIN_PRELOAD / SFSE_PLUGIN_LOAD   (src/main.cpp)
                                   │
                            Core/Plugin.cpp        entry glue, SFSE messages
                                   │
                         Runtime/Runtime          coordinates everything below
       ┌──────────────┬──────────────┬──────────────┐
       │              │              │              │
     Core/          Views/         Bridge/       Settings/
 Config, Paths   manifests,      MessageBridge   schemas and
                presentation,    retained state  persistence
                reveal policy
       │              │              │              │
       └──────────────┴──────────────┼──────────────┘
                                    │
                ┌───────────────────┼────────────────────┐
                │                   │                    │
             Render/            Composite/            Input/
          WebView2 renderer   D3D12 compositor   engine integration
                │                   │                    │
                └───────────────────┴────────────────────┘
                                    │
                                  API/
                         BridgeApi and PapyrusApi
```

The public extension APIs hang off the bridge rather than the render path:

- `API/` is the public extension surface — `BridgeApi` backs the exported
  `OSFUI_RequestBridge` C ABI ([native-plugin-api.md](native-plugin-api.md)),
  `PapyrusApi` backs the shipped `OSFUI` script's natives
  ([authoring-dynamic-data.md](authoring-dynamic-data.md)). Both marshal onto
  the main thread and derive a caller's mod id from the trusted source rather
  than the payload.

### Data flow per frame

1. An SFSE permanent task (registered in `Core/Plugin.cpp`) runs on the engine's render-graph workers and posts one coalesced `Runtime::Tick(dt)` through `RE::BSService::TaskQueue`. The queue drains Tick on the game main thread; if BSService cannot enqueue yet, the worker drops that notification and retries next frame rather than taking the queue's unsafe inline fallback. Tick self-times on the main thread and clamps `dt` to 100 ms.
2. `WebView2HostWebRenderer::Update(dt)` advances the web content.
3. The browser host publishes frames through a shared D3D12 texture ring; `WebView2HostWebRenderer::Render()` returns the frame-ready slot and fence serial.
4. On a closed-to-open edge, `Views/ViewRevealGate` keeps the compositor hidden until it observes a fresh frame, a known output size, and a texture matching that size. `Runtime` submits gate-approved frames and performs the real compositor side effects. If the bounded wait expires, Runtime closes the presentation and immediately releases focus, input, pause, and cursor ownership.
5. `D3D12Compositor::Submit(frame)` records the approved slot; the overlay is drawn later, inside the engine's Scaleform UI pass (see *How the D3D12 compositor works*), sampling the shared texture directly with no CPU readback or upload.

The same Tick is where the bridge does its main-thread work: it drains the
Papyrus and native-plugin queues (retained state, events, replies), expires
deferred requests past their deadline, and applies plugin registrations —
all before `Update()` flushes the per-view outbound queues, so a value a mod backend
published this tick reaches the page in this tick's frame.

### Message bridge

All native↔web traffic is JSON text through `MessageBridge` (web bridge protocol
2.0; the rationale is [mod-api-2.0-design.md](mod-api-2.0-design.md)). There
are four verbs, chosen by semantics rather than by transport, and exactly one
envelope shape each:

```
web  -> native   { kind: "send",    name, payload }           one-way, no completion
                 { kind: "request", name, id, payload }       settles exactly once

native -> web    { kind: "ready",   payload }                 handshake answer
                 { kind: "state",   mod, key, value }         named value, latest-wins
                 { kind: "event",   name, payload }           one-shot happening
                 { kind: "reply",   id, payload }
                 { kind: "error",   id, payload: { code, message } }
```

Routing metadata (`kind` / `name` / `id` / `mod` / `key`) sits **beside** the
payload, never inside it. 1.x put the command name *in* the payload and the
helper assembled it with `Object.assign`, so a payload field could override
routing; the split makes that structurally impossible.

The bridge has no built-in knowledge of any feature. It transports envelopes
and dispatches names through two registries that are disjoint by construction:

- `RegisterSend(name, fn)` — pure notifications. Nothing is settled, nothing
  is echoed. Wanting an outcome means it should have been a request.
- `RegisterRequest(name, fn)` — endpoints that settle exactly once, with
  `Respond` (payload), `Reject` (stable code + sentence), or `Defer` (take
  ownership of the correlation id and settle later through
  `RespondTo`/`RejectTo`). A handler that returns having done none of the
  three is a platform bug: the caller gets `internal` and the log gets an
  ERROR, because "never settled" is the one failure a caller cannot tell
  apart from a hang.

Registering one name as both strict kinds is refused and logged, because the
kind is what callers dispatch on. Kind enforcement is structural in both
directions: a `request` naming a strict send endpoint is rejected
`wrong-endpoint-kind`, and a `send` naming a request endpoint is **dropped** and
surfaced. Native ABI 2.0 follows the same strict split through `RegisterSend`
and `RegisterRequest`; routing metadata is never injected into payloads.

Endpoints come from three places:

- core — platform and window endpoints only (`close`, `setVisible`, `log`,
  `ping`, `menu.open`/`menu.close`, …), registered during OSF UI runtime
  initialization;
- each feature module — its own namespace, via `IUiModule::RegisterEndpoints`
  (settings registers `settings.set` / `settings.reset` / `settings.captureKey`);
- a third-party SFSE plugin — through `BridgeApi`, restricted to
  `<author>.<modname>.<name>` names ([security-model.md](security-model.md)
  rule 5).

There is no generic "call native" endpoint, and no name a view message can
turn into a function pointer.

Deferred requests are tracked with their owning view: bounded per view (64
concurrent, then `request-capacity`), swept each Tick against a 30 s OSF UI
OSF UI runtime deadline (`no-response`), and reaped when their view goes away. The
client timer in the JS helper (default 10 s, `timeout`) is deliberately shorter
and deliberately a *different* code: `timeout` means the page gave up,
`no-response` means the mod backend or OSF UI runtime handler never answered,
and collapsing them would hide which side is broken.

### Lifecycle: the handshake is page-initiated

A fresh document greets the bridge with `osfui.hello`, and that is the only
boot path. First open, F5, dev hot-reload and crash-recovery reload are the
same sequence:

```
document loads
  └─ shared bridge helper sends  { kind: "send", name: "osfui.hello" }
       └─ bridge sends  ready
            └─ hello hook: full state replay (platform keys + owning mod's keys)
                 └─ event gate opens; queued events flush; the view renders
```

Because the document asks, the OSF UI runtime never has to guess whether a
runtime-initiated greeting was consumed. The several 1.x `SendRuntimeReady`
call sites and their coordination with the browser host's DOM-delivery gate are
gone. The browser host still uses its internal `domSeen` gate to queue script
delivery across navigation; it no longer decides when the bridge handshake
begins. The OSF UI runtime's whole obligation is to answer hellos, in order,
with `ready` then state.

The ordering guarantee (`ready` < state < events) is structural rather than a
convention every call site must remember, because `Runtime` installs one hello
hook (`SetHelloHook` → `Runtime::OnViewGreeted`) and the bridge calls it
between sending `ready` and opening the gate:

- **Events** emitted after a document instance is created but before its first
  greeting are queued per view (bounded at 64, dropping the *oldest* — the
  newest happenings are the ones still worth delivering). The greeting does
  not clear them; it flushes them after `ready` and state. That preserves the
  native ABI's message-before-first-paint guarantee. Creating or recreating a
  view arms a fresh gate and clears any queue belonging to the prior document.
  Once an event is delivered, it is never retained for a later document.
- **State** to a view that has not greeted is *dropped*, not queued: the
  replay hands that document every current value anyway, and queueing risks
  delivering a stale value after a newer one.

A greeting also means the previous document is gone, so `OnViewGreeted` drops
the sticky input grants (`osfui.gamepadRaw`, `osfui.handleBack`) that document
asserted — which covers an F5 the OSF UI runtime is never told about.

### Retained mod state

`Bridge/RetainedStateStore` holds mod-backend-owned values scoped by publishing
mod and replayed to each of that mod's fresh documents. It does not store an
independent value per view. This is the systemic fix for the blank-after-F5
bug class: a mod backend that knows when a value changes publishes it once,
and the OSF UI runtime owns the replay, so no view has to re-request anything
and no mod backend has to listen for a view-defined hello.

Both mod backend types land in the same store, which is what makes the
four-verb grid square across Papyrus, the native ABI and the platform:

| | one-shot happening | value that stays true |
|---|---|---|
| Papyrus | `SendViewEvent` | `SetView*` |
| Native plugin (C ABI) | `SendToWeb` | `SetViewState` |
| Platform | `Emit` / `EmitAll` | `PublishState` / `PublishStateAll` |

Keys are matched case-insensitively (a Papyrus key arrives through
`BSFixedString` interning, which returns the first casing the process saw, so
the script's literal spelling does not survive) while the publisher's original
casing is kept for delivery. Values are complete per key, never deltas, so a
replay and a live update are the same message. Each mod is capped at 64 keys.
Papyrus state is session-scoped — its values can hold form identities, which
are meaningless after a game load — while native-plugin state is not, because
wiping a plugin's HUD configuration on every load would be the bug.

Each publish sends mod state immediately to the publishing mod's instantiated
views, resolved fresh on that publish, while the retained value remains ready
for the mod's future documents. There is no subscriber set to prune or to go
stale. The platform's own registries are state keys on the
`osfui` mod: `osfui/settings`, `osfui/views`, `osfui/diagnostics`, `osfui/keybindings`,
`osfui/input-context`, `osfui/i18n` (computed per view —
a document's catalog is its owning mod's). The original registries replaced the 1.x
`*.get` requests, each of which was a read with the invisible side effect of
subscribing the caller — which is the definition of state, not of a read.

### OSF UI runtime-detected protocol faults have a route back to the page

View-caused protocol faults the page would otherwise never hear about (a
dropped send, an unknown endpoint, or a malformed envelope) go through the
bridge's protocol-fault callback, which `Runtime` wires to `OnProtocolFault`.
In developer mode the offending view is handed an `osfui.debug.error` event,
so the failure lands in that page's own console and therefore in F12 DevTools.
In developer mode those faults are returned only to their originating page;
recurring faults also become a bounded System Health issue, while release
builds keep the individual rejection in the native log. A mod backend or OSF UI
runtime handler missing its deadline is also reported to the waiting page as
`no-response`. Details are in [troubleshooting.md](troubleshooting.md) and
[logging.md](logging.md).

### Feature modules ("apps" on the platform)

Features are `IUiModule`s (`Runtime/UiModule.h`). `IUiModule` is a uniform lifecycle fan-out: the OSF UI runtime drives every module through the same points — `OnStart()` (applies persisted state at load), `RegisterEndpoints(bridge)` (wire its own send/request endpoints), `OnBridgeDown()`, `OnViewDestroyed()` — from one loop in registration order, rather than a per-module call at each site. `HealthRegistry` owns the bounded session-local issue model and publishes `osfui/diagnostics`; `RuntimeHealthCoordinator` reconciles settings, input, view, browser-host, compositor, and compatibility observations into it. Neither component exports or uploads a report. It is not a decoupling seam: the OSF UI runtime still owns and reaches through the concrete module types directly.
`Runtime::InitializeFeatureModules()` is the composition root - the one place that names concrete modules and injects their dependencies

### Views

`ViewManager` does a **two-level** scan of `<data>/views/<modId>/<viewName>/manifest.json`. The first level is a mod namespace (its folder name must pass the mod-id grammar; `shared/` is skipped as the asset kit, and a manifest found at the first level is rejected as the pre-1.0 flat layout). The second level is the view. **The path is the identity**: the qualified view id is `<modId>/<viewName>`, derived from the folder, never from the file — a manifest declares no id at all (a legacy `id` field is ignored), so a manifest cannot claim another mod's namespace. Subfolders without a `manifest.json` are ignored, so a mod can keep shared assets beside its views.

A `ViewManifest` declares metadata such as the entry page, size,
transparency, and a permission block that defaults to deny (`nativeBridge`,
`filesystem`, `network`). It declares no identity field; the path supplies the
mod id and view name. Manifest entries may not point outside the view folder.

Discovery does not create browser content, and there is no configured view
list: every valid manifest is discovered (sorted by qualified id, so creation
order and z tie-breaks are deterministic), and its view is instantiated on
first open. The only startup instances are HUDs whose effective auto-start is
on. That policy is the player's: `ViewPolicyStore` persists per-HUD
choices from Mod Settings (`state/view-policy.json`, temp-file replaced,
quarantined to `.bad` when malformed, retained for uninstalled views); the
manifest's `openOnStart` is only the author's HUD auto-start default,
`hub:false` views are not catalog-visible and are never eligible, and menus
never auto-start from discovery. Explicit native-plugin `RegisterView` is a
separate entry path: there, `openOnStart` means open on registration, including
for a menu.

Once instantiated, a view's document remains resident until Starfield exits;
ordinary close only hides it. This deliberately trades session memory for a
smaller lifecycle: there is no idle suspension, TTL, hidden-view cap, or LRU
reclamation. If the browser host dies, recovery recreates every instantiated
view in the replacement host and leaves the overlay closed for the player to
reopen. A document that exhausts its own load-recovery budget is still torn
down as a terminal failure rather than retained as a usable view.

### Frontend build

The built-in views are generated, not hand-written. `frontend/` is a Vite + TypeScript + Preact project whose ignored build artifact is `build/frontend/views/`:

```
frontend/src/  ──(npm run build)──►  build/frontend/views/  ──► xmake install / MO2 redeploy / package.ps1
```

- Per view the build emits `main.js` and `style.css`, and copies `index.html` + `manifest.json` through unprocessed — Vite's HTML pipeline would inject `type="module"` and `crossorigin` and hash asset names, all three of which break the shipped contract.
- `views/shared/osfui.css` and `views/osfui/padnav.js` are copied verbatim. The shipped `views/shared/osfui.js` is deterministically composed from the byte-unchanged 2.0 core and, during 2.0.x, an isolated guarded v1 façade; both shapes are asserted on every build (`frontend/COMPATIBILITY.md`).
- Output filenames are stable — no content hashes — so public asset paths and installed layouts remain deterministic.
- The generated tree is **build output and is not committed**: it is ignored, regenerated on every build, and never hand-edited. `xmake build` and `xmake install` run the frontend builder before consuming it, while release packaging installs the locked npm dependencies first. Node is a developer/build dependency, never a player dependency.

Nothing in the OSF UI runtime is frontend-aware: it discovers whatever manifests are on disk, exactly as it does for a third-party mod's hand-authored view. The shipped `views/shared/osfui.js` helper is the one thing every view — built-in or third-party — loads in common, which is why it is versioned with the protocol rather than with the views.

## How the web renderer and browser host fit

`WebView2HostWebRenderer` launches `osfui_webview2_host.exe` outside the
game's process tree, communicates over a framed named pipe, and receives
browser frames through shared D3D12 textures. Keeping Chromium out of process
avoids MO2/USVFS injection into `msedgewebview2.exe`. The game creates the
owner-only server pipe before launch, and both ends compare the kernel-reported
peer PID with the expected process before accepting the session. Starfield-side
writes run only on a bounded transport worker; the reader applies a total hello
deadline and a heartbeat deadline so an alive but stalled browser host is recoverable.

The browser host owns one WebView2 environment and one composition controller
for each instantiated view. These remain separate browsing instances for fault and DOM
isolation; Chromium decides their renderer-process assignment. Lazy views have
no controller until first use; after that, browser resource use can grow with
the number of distinct views opened during the session.

Page → browser-host traffic is bounded in the browser host before it can
allocate anything downstream: a single page message over 64 KiB is dropped,
and a view exceeding
128 messages per second has the excess dropped, each warned once per view.
That applies to every document, including one whose manifest denies
`nativeBridge`, because any page can call `chrome.webview.postMessage`
directly.

## How the D3D12 compositor works

`D3D12Compositor` draws on the game's own D3D12 device. Frames are sampled directly from the browser host's shared texture ring with no CPU readback or upload. The overlay quad is drawn *inside the engine's Scaleform UI pass* (`Composite/UiPassSeam`, hooked at ScaleformEnd) so frame generation (FSR3 / DLSS-G) paces the overlay like native UI. The compositor does not hook `IDXGISwapChain::Present`: Submit adopts shared rings on the tick thread, while the seam reports output dimensions and identifies the transparent `COPY_SOURCE` UI hand-off used when frame generation is active. This keeps OSF UI outside Present chains owned by OptiScaler, Streamline, Steam, RTSS, ReShade, and similar tools.

Remaining open areas: alternate UI-target formats and broader in-game validation across frame-generation and external-overlay combinations.

## Lifetime

- `SFSE_PLUGIN_LOAD` → `Runtime::Get().Initialize()`: paths → config → views → renderer → compositor → bridge → input config. The bridge object exists from that point, but the conversation with any given document starts when *that document* greets it.
- SFSE has no shutdown callback, so engine-facing singletons intentionally have process lifetime and do not run destructors from DLL detach.
- Normal in-session recovery and explicit renderer stops use a synchronized lifecycle: stop new sends, give the background writer 250 ms to deliver shutdown, cancel all overlapped pipe I/O, join transport workers, then wait a bounded interval for the verified browser host and terminate it only if necessary.
- During process exit the browser host independently watches the Starfield process handle; correctness never depends on destructor ordering after Windows begins process teardown.
