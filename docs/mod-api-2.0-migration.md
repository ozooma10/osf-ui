# OSF UI mod API 2.0 — migration

Read this when your mod stopped working after updating OSF UI. It's the mechanics half of [mod-api-2.0-design.md](mod-api-2.0-design.md), which covers *why* the API is shaped this way.

**The raw 2.0 web protocol is a hard break**, but views that used the shipped 1.x helper do not have to migrate immediately. A manifest declaring `targetVersion` below `2.0` receives a compatibility façade over the 2.0 transport; migrate to the four-verb surface to adopt new features and remove the shim on your own schedule. Views that bypassed the helper and wrote 1.x envelopes directly must migrate. The separately versioned native C++ ABI stays additive at 1.8, so already-compiled 1.x DLLs still connect.

| Artifact | Breaks? | How you find out |
|---|---|---|
| View (`views/<modId>/<viewName>/`) | **Compatibility available** | A declared pre-2.0 target receives the established callable `available()` surface and helper aliases over protocol 2.0. Raw `postMessage`/`onMessage` consumers and undeclared legacy views must migrate. |
| Native SFSE plugin (`sdk/OSFUI_API.h`) | **No** | ABI 1.8 appends `SetViewState`; binaries built against 1.0–1.7 keep their vtable and command behavior. |
| Papyrus script | **Partly** | Six removed natives (`PushToView`, `PushFormsToView`, `RegisterForViewActions{,Static,Args,ArgsStatic}`). Calls fail to resolve in the VM and error in `Papyrus.0.log`. Everything else keeps its name. |
| Settings schema (`settings/<modId>.json`) | **No** | Declarative data. An `action` row uses a request; an older `RegisterCommand` handler stays callable through its compatibility auto-ack path. |
| Player values, localization catalogs | **No** | Same files, same format. |

---

## 1. View authors — the JS helper

`frontend/src/shared-kit/osfui.js` (shipped to `SFSE/Plugins/OSFUI/views/shared/osfui.js`; typed in `sdk/osfui.d.ts`).

The whole surface is four verbs — `send`, `request`, `on`, `state` — plus three sugar namespaces (`papyrus`, `i18n`, `theme`) that add ergonomics and never new semantics. Every 1.x alias is gone rather than deprecated.

### 1.1 Member-by-member

| 1.x member | 2.0 replacement | Break |
|---|---|---|
| `osfui.available()` | `osfui.available` — a **property** | **LOUD** — `TypeError: osfui.available is not a function` |
| `osfui.ready` (never rejected) | same promise, now **rejects** `no-bridge` in a plain browser instead of hanging | loud in the standalone case (unhandled rejection at your `await`) |
| `osfui.send(command, fields)` | `osfui.send(name, payload)` | signature-compatible, **wire changed** (§2); name and payload no longer merge |
| `osfui.emit(command, fields)` | `osfui.send(name, payload)` | **LOUD** |
| `osfui.call(command, fields, opts)` | `osfui.request(name, payload, opts)` | **LOUD** |
| `osfui.request(...)` → the **envelope** | `osfui.request(...)` → the reply **payload** | **SILENT** — see §1.2 |
| `osfui.action(name, ...args)` | `osfui.papyrus.send(name, ...args)` | **LOUD** |
| `osfui.papyrus.action(name, ...args)` | `osfui.papyrus.send(name, ...args)` | **LOUD** |
| `osfui.papyrus.request(name, ...args)` | unchanged (still unwraps to the script's value) | none |
| `osfui.viewReady()` | `osfui.markReady()` | **LOUD** |
| `osfui.on(type, fn)` | `osfui.on(event, fn)` — **events only**; replies never fire handlers | name survives; see §1.3 |
| `osfui.data.on(key, fn)` | `osfui.state.on("<mod>/<key>", fn)` | **LOUD** — `osfui.data` is `undefined` |
| `osfui.data.get(key)` | `osfui.state.get("<mod>/<key>")` | **LOUD** |
| `osfui.t(address, english, vars)` | `osfui.i18n.t(...)` | **LOUD** |
| `osfui.localize(root)` | `osfui.i18n.localize(root)` | **LOUD** |
| `osfui.locale()` | `osfui.i18n.locale` — a **property** | **LOUD** |
| `osfui.i18nReady` | `osfui.i18n.ready` | **LOUD** at `.then(...)`; see §1.3 |
| `osfui.applyAccent(el, hex)` | `osfui.theme.applyAccent(el, hex)` | **LOUD** |
| `osfui.onMessage` | still helper-owned; do not assign it | unchanged |
| — | `osfui.state.on/get` (new fourth verb) | — |
| — | `localStorage["osfui:trace"] = "1"` envelope tracing | — |

The typed façade the built-in views use, `frontend/src/lib/bridge.ts`, mirrors this exactly: `available()`, `send()`, `request()`, `on()`, `onAny()`, `state(key, fn)`, `peek(key)`, `markReady()`, `ready()`, `i18nReady()`, `locale()`, `t()`, `applyAccent()`, `papyrusSend()`, `papyrusRequest()`. `emit()`, `call()` and `viewReady()` are gone from it too.

### 1.2 The one silent break

> **`osfui.request()` now resolves the reply *payload*, not the whole envelope.** In 1.x it resolved `{ type, payload, requestId }` and you read `reply.payload.x`. Nothing throws, nothing logs — `reply.payload` is simply `undefined`. **Grep every `request(` call site and delete one `.payload` hop.** This is the only 2.0 change that gives you no signal at all.

```js
// 1.x
const reply = await osfui.request("game.get");
render(reply.payload.calendar);

// 2.0
const data = await osfui.request("game.get");
render(data.calendar);
```

Everything else fails at the point of use: a removed member throws `TypeError`, a removed *request* endpoint rejects with a typed `code` that the helper prints to the page console with an `[osfui]` prefix, and a removed *send* endpoint is dropped by the host and surfaced — as an `osfui.debug.error` event in devMode, and as a `view.protocol-misuse` health card after 10 offences in release builds (`Runtime::OnProtocolMisuse`, `kMisuseThreshold = 10`, diagnostics source `"views"`).

### 1.3 Two quieter degradations worth grepping for

Both are reachable through names that still exist, but change what your handler receives.

- **`on()` handlers get one argument now.** 1.x called `fn(message.payload || {}, message)`; 2.0 calls `fn(payload)`. If you destructured the second parameter it's `undefined`. Moot for platform events (the 1.x names are gone); it matters for mod-defined `<mod>.<name>` events fed by a plugin's `SendToWeb`.
- **Request replies no longer fan out to `on()` subscribers.** 1.x dispatched a correlated reply to `on()` *and* settled the promise. 2.0 keeps the channels separate: a reply settles exactly one promise and reaches no event handler. The 1.x pattern is obsolete anyway — the registries it existed for are now state keys that replay unasked.
- *Footnote on `osfui.i18nReady`:* the member is gone, so `osfui.i18nReady.then(…)` throws, but `await osfui.i18nReady` resolves `undefined` immediately. Nothing renders wrong: the helper localizes the document itself when the `osfui/i18n` state key arrives (`deliverState()` → `localize(document)`), so `data-i18n` markup still applies. Any *manual* `osfui.t(...)` in the same file throws.

### 1.4 Code you should delete, not port

A correct 2.0 view has **zero lifecycle code**:

- "On ready, re-request my data." State is replayed to every fresh document after `ready` and before the first event. Subscribe once, at module scope.
- A page-level `ready`/`hello` action fired at your own Papyrus script so it re-pushes. Gone on both sides — see §4.
- Any handshake re-asserting subscriptions after F5. `state.on()` replays synchronously on subscribe.
- Re-asserting `osfui.gamepadRaw` / `osfui.handleBack` from a *reload handler*. There is no reload handler; assert them from ordinary setup code, which runs on every document. (`Runtime::OnViewGreeted` clears `_gamepadRawViews` and `_backOwnerViews` on every greeting.)

---

## 2. View authors — the wire

Only relevant if you talk to `postMessage`/`onMessage` directly. Source of truth: `src/runtime/MessageBridge.{h,cpp}`.

**1.x, web → native** — one shape, routing *inside* the payload:

```json
{ "type": "ui.command", "requestId": "q1",
  "payload": { "command": "settings.set", "mod": "acme.mymod", "key": "x", "value": 1 } }
```

**2.0, web → native** — routing metadata *beside* an opaque payload, so a payload field can never override routing:

```
{ kind: "send",    name, payload }
{ kind: "request", name, id, payload }     // id REQUIRED on request, FORBIDDEN on send
```

**2.0, native → web** — five shapes, one per concept:

```
{ kind: "ready",  payload: RuntimeInfo }        // answer to osfui.hello
{ kind: "state",  mod, key, value }             // latest-wins, complete per key, replayed
{ kind: "event",  name, payload }               // one-shot, never replayed
{ kind: "reply",  id, payload }
{ kind: "error",  id, payload: { code, message } }
```

`RuntimeInfo` = `{ game, plugin, version, bridgeVersion, view, mod }`. `view` is this document's qualified id and `mod` its owning mod — the prefix to build your own `"<mod>/<key>"` state keys with.

Envelope rules that went from lenient to strict:

- A request `id` must be a non-empty string of at most **64 characters**. 1.x demoted a malformed or oversized id to fire-and-forget; 2.0 answers a hard `invalid-request`, because silent demotion turned a client bug into a request that never settles (`kMaxRequestIdLength`).
- An `id` on a `send` is `invalid-request`.
- A non-object `payload` is `invalid-request`; absent or `null` is accepted and becomes `{}`.
- Endpoint kind is enforced structurally: `request()` naming a send endpoint rejects `wrong-endpoint-kind`; `send()` naming a request endpoint is dropped and surfaced.

### The handshake is page-initiated, and it is the only boot path

```
document loads
  └─ helper sends { kind:"send", name:"osfui.hello" }
       └─ bridge sends `ready`
            └─ hello hook replays state (platform keys + this mod's retained state)
                 └─ the view's event gate opens and queued events flush
```

First open, F5, dev hot-reload and crash-recovery reload are literally the same sequence. You can rely on:

- `ready` precedes all state for that document; all replayed state precedes the first event.
- Events emitted before a document greets are **queued** per view, bounded at **64, drop-oldest** (`kMaxQueuedEventsPerView`) — this preserves the native ABI's message-before-first-paint guarantee.
- State published to a view that hasn't greeted is **dropped**: the replay covers it, so there's nothing to queue and no ordering to get wrong.
- A second `hello` from the same view id means a new document: the queue is cleared (replaying one-shot events into a fresh page would re-fire their effects) and the full state replay runs again.

Request rejection codes: `no-bridge` (local), `timeout` (client timer, default 10 s; `timeoutMs: 0` disables **only** the client timer), `no-response` (backend missed the host-side 30 s `kRequestDeadline`), `wrong-endpoint-kind`, `unknown-endpoint`, `invalid-request`, `request-capacity` (64 concurrent deferred requests per view), `internal` (a request endpoint returned without settling) — plus whatever the handler rejected with.

---

## 3. Endpoint reclassification

The four-verb model dissolves 1.x's subscribe-on-read wart: `settings.get`, `views.get`, `i18n.get` and `diagnostics.get` were requests whose real job was to *subscribe* you. Reads-with-replay are exactly what state is, so those four registries became state keys and the requests were deleted.

### 3.1 State keys — `osfui.state.on(key, fn)`

| Key | Replaces | Value |
|---|---|---|
| `osfui/settings` | `settings.get` → `settings.data` | the whole settings registry (`SettingsData`) |
| `osfui/views` | `views.get` → `views.data` | every discovered surface with live open/focus/load state |
| `osfui/diagnostics` | `diagnostics.get` → `diagnostics.data` | the System Health snapshot |
| `osfui/i18n` | `i18n.get` → `i18n.data` | `{ mod, locale, strings }` — **computed per view**, the owning mod's catalog |
| `osfui/handoff` | the `handoff.state` push | (platform-private) first-load handoff state |

Plus every mod key a backend publishes: Papyrus `SetView*` and the ABI's `SetViewState` both land under `"<yourModId>/<key>"`.

### 3.2 Events — `osfui.on(name, fn)`

`settings.changed`, `settings.persisted`, `settings.captured`, `ui.hotkey`, `ui.visibility`, `ui.gamepad`, `osfui.debug.error` (devMode only), and every mod event `"<mod>.<name>"` from Papyrus `SendViewEvent` or the ABI's `SendToWeb`.

The first six names are unchanged from 1.x. What changed: they are now *only* events — nothing else arrives on this channel.

### 3.3 Sends — `osfui.send(name, payload)`

`osfui.hello`, `close`, `setVisible`, `view.ready`, `log`, `osfui.gamepadRaw`, `osfui.handleBack`, `osfui.handoffRetry` (platform-private), `papyrus.call`, `papyrus.send`.

### 3.4 Requests — `osfui.request(name, payload)`

`menu.open`, `menu.close`, `setViewHidden`, `ping`, `game.get`, `settings.set`, `settings.reset`, `settings.captureKey`, `osfui.openModPage`, `osfui.openLogFolder`, `osfui.setViewAutoStart`, `papyrus.request`.

**Kind changes to watch for** — these names survived but moved from command to request, so a 1.x `osfui.send('menu.open', …)` is now dropped and surfaced rather than executed:

| Endpoint | 1.x | 2.0 |
|---|---|---|
| `menu.open` / `menu.close` | command | request, resolves `{}`, rejects `unknown-view` |
| `setViewHidden` | command | request, resolves `{}` |
| `osfui.openModPage` | command | request, rejects `shell-failed` |
| `osfui.openLogFolder` | command | request, rejects `no-log-folder` \| `shell-failed` |
| `ui.papyrusRequest` | request | renamed `papyrus.request` |
| `ui.action` | command | renamed `papyrus.send` |

**Settlement-shape changes:**

- `settings.set` **rejects** on failure (`forbidden`, `unknown-setting`, `read-only`, `invalid-value`) instead of resolving `settings.ack { ok:false }` the caller had to remember to inspect, and resolves `{ mod, key, value }` with the **post-clamp committed** value, so clamped and accepted are distinguishable without a re-read (`src/runtime/SettingsModule.cpp`).
- `settings.reset` resolves `{}`. The refreshed registry reaches every view — the caller included — through `osfui/settings`, rather than arriving by a different route for the caller alone.
- `settings.captureKey` settles in **machine time**: resolves `{ armed: true, mod, key }` or rejects `capture-busy` / `forbidden` / `not-rebindable`. The captured key (or cancellation) arrives afterwards as the `settings.captured` **event**. There is no `timeoutMs: 0` usage anywhere any more — a request that waits on a human is the wrong shape.
- `ping` resolves `{}` (was a `runtime.pong` message); `game.get` resolves `GameData` directly (was `game.data`).

### 3.5 Deleted endpoints

| Deleted | Why / what to use |
|---|---|
| `hud.show`, `hud.hide` | Pure aliases of `menu.open`/`menu.close` — registered to the *same handler lambdas* in 1.x. Use `menu.open` / `menu.close` as **requests**. |
| `osfui.textFocus` | A registered **no-op** in 1.x, purely so a pre-session-focus view wouldn't trip `unknown-command`. An unknown send is now a dev-only debug event, so it bought nothing. Nothing replaces it. |
| `settings.get`, `views.get`, `i18n.get`, `diagnostics.get` | The four subscribe-on-read requests. Use the state keys in §3.1. |
| `ui.action` | Renamed `papyrus.send`. |
| `ui.papyrusRequest` | Renamed `papyrus.request`. |

### 3.6 Deleted native → web message types

The `kind` field replaces the whole `type` taxonomy: `runtime.ready` (now `kind:"ready"`), `ui.result`, `ui.error` (now `kind:"reply"` / `kind:"error"`), `settings.ack`, `settings.data`, `views.data`, `i18n.data`, `diagnostics.data`, `data.push`, `data.state` (now `kind:"state"`), `papyrus.result`, `runtime.pong`, `game.data`, `handoff.state`.

---

## 4. Papyrus authors

`data/Scripts/Source/OSFUI.psc`, natives in `src/api/PapyrusApi.{h,cpp}`.

Papyrus keeps its 1.5 names on purpose: renaming `ListenForViewActions` / `OnOSFUIViewAction` would churn exactly the mods that already migrated, in the one language where migrating means recompiling `.pex` files.

| 1.x | 2.0 | Break |
|---|---|---|
| `PushToView(mod, key, values)` | `SetView*` for state, `SendViewEvent` for happenings | **LOUD** — the native no longer binds; the call fails to resolve and errors in `Papyrus.0.log`, unwinding that stack |
| `PushFormsToView(mod, key, forms)` | `SetViewForms(mod, key, forms)` | **LOUD** |
| `RegisterForViewActions(receiver, fn, mod)` | `ListenForViewActions(receiver, mod)` → `OnOSFUIViewAction(string, string[])` | **LOUD** |
| `RegisterForViewActionsStatic(script, fn, mod)` | `ListenForViewActionsStatic(script, mod)` | **LOUD** |
| `RegisterForViewActionsArgs(...)` | `ListenForViewActions(...)` | **LOUD** |
| `RegisterForViewActionsArgsStatic(...)` | `ListenForViewActionsStatic(...)` | **LOUD** |
| — | **`SendViewEvent(mod, name, args)`** (new) | emits `"<mod>.<name>"` with payload `{ args }`; never cached, never replayed |
| `SetViewBool/Int/Float/String/Bools/Ints/Floats/Strings/Forms` | unchanged | none — but the wire shape is now `kind:"state"` and the view consumes it with `osfui.state.on()` |
| `ListenForViewRequests{,Static}`, `ReplyView*`, `RejectViewRequest` | unchanged | none |
| `GetFormById`, `GetFormsById`, `Unregister`, `OpenMenu`, `CloseMenu` | unchanged | none |
| `RegisterForSettingChanges{,Static}`, `RegisterForHotkey{,Static}`, `Get*`/`Set*`/`Reset` | unchanged | none |

- `PushToView` was **transient like an event but shaped like state** — which is why every mod using it needed a page-level `ready` action and a matching re-push. Delete both halves.
- Papyrus `SetView*` state stays **session-scoped**: values can hold form identities, which don't survive a game load. `Papyrus::TakeSessionReset()` reports the load and the runtime drops those entries (`ViewStateStore::ClearSessionScoped`). Republish after a load, exactly as in 1.5. Native `SetViewState` is *not* session-scoped.
- Retained-state mechanics carry over: latest-wins per `mod`+`key`; case-insensitive keys (Papyrus `BSFixedString` interning hands back the first-seen casing); at most **64 keys per mod**; `None` form slots preserved as JSON `null` so a parallel values key stays index-aligned.
- C++-side names moved with the concept: `ViewPush` → `ViewState` (`{ mod, key, value }`) + `ViewEvent` (`{ mod, name, args }`); `DrainViewPushes` → `DrainViewState` + `DrainViewEvents`; `ReplayViewState` removed entirely because the runtime owns the cache now (§6.3).

---

## 5. Native plugin authors

`sdk/OSFUI_API.h` (+ the optional `sdk/OSFUI_JSON.h` facade), host side in `src/api/BridgeApi.{h,cpp}` and `src/api/Exports.cpp`.

### 5.1 Additive ABI 1.8

```cpp
inline constexpr std::uint32_t kBridgeAPIVersion = (1u << 16) | 8u;
```

`OSFUI_RequestBridge` compares the major, so every caller built against ABI 1.x receives the bridge. `SetViewState` is appended after the complete 1.7 virtual interface and exposed as `Feature::kViewState = 8`; no earlier slot moves. The 1.8 `Client` checks the running minor before using that tail method; an already-compiled older Client never knows or calls it.

### 5.2 What a 1.x plugin experiences

It connects normally. Existing settings, hotkey, view, diagnostics, request, event and readiness calls keep their slots and feature minors. `RegisterCommand` keeps its behavioral contract: `send()` invokes it one-way; `request()` injects `requestId` into the callback payload and receives an automatic `{ ok: true, command }` reply after return. That only proves dispatch, so new result-bearing endpoints should use `RegisterRequest`.

A genuinely different ABI major is still refused and may raise the bounded, deduplicated `compat.legacy-api` diagnostic — that path doesn't apply to any 1.x caller.

A view whose manifest declares a `targetVersion` below `2.0` is navigated with the 1.x helper façade selected. It is not reported as unhealthy merely for using the supported compatibility path. An *undeclared* `targetVersion` is deliberately treated as current: after parsing it is indistinguishable from "declared and unparsable", and guessing would force the legacy helper shape onto every undeclared view.

Both codes ride on `CompatibilityTarget`, which grew a `code` field defaulting to `"compat.needs-newer-osfui"` (`src/runtime/DiagnosticsReconciler.h`). Every compat producer **must** flow through the one `SyncCompatibility` call, because `ResolveMissing` sweeps by `source` — two producers writing `source: "compat"` would each resolve the other's issues on every pass.

### 5.3 Source changes

| 1.0–1.7 | 1.8 | Break |
|---|---|---|
| command requests carried an injected `"requestId"` | unchanged | none |
| the host auto-acked after a command request returned | unchanged; explicit `RegisterRequest` preferred for meaningful results | none |
| a command that wanted to answer replied out-of-band with the injected id | `RegisterRequest` + `Request::Respond` / `Reject` (unchanged since ABI 1.7) | — |
| `SendToWeb(viewId, type, payloadJson)` | unchanged signature; now encodes `{ kind:"event", name: type, payload }` | none — the view still reads it at `osfui.on(type)` |
| — | **`SetViewState(modId, key, payloadJson)`** (new vtable tail) | additive |
| `Feature` gates decide whether a call exists | unchanged; `Feature::kViewState = 8` | none |
| — | `BridgeApi::TakeViewStateOps()` (new drain) | — |

`SetViewState` is the systemic fix for the blank-after-F5 bug class on the native side. Publish once and the runtime replays it to every document of your mod:

- latest-wins per key, and the value is **complete** — never a delta;
- any JSON *value*, validated synchronously;
- keys matched case-insensitively, at most 64 per mod;
- **not** session-scoped — wiping a HUD's configuration on every save load would be a bug. (Papyrus `SetView*` state *is*, because it can hold form identities.)

### 5.4 Settings-schema `action` rows should use `RegisterRequest`

An `action` row's `command` was a fire-and-forget command with an ack convention in 1.x. In 2.0 the Mods surface fires it as a **request** (`frontend/src/views/osfui/settings/App.tsx`, `runAction`, 5 s timeout) and renders the settlement: `Respond` with an object — an optional string `message` becomes a toast, `{}` is a silent success — or `Reject` with a code and sentence, rendered as an error toast. An older `RegisterCommand` handler still runs through the ABI compatibility path and auto-acks as a silent success, but can't express a meaningful result. A `RegisterRequest` handler that never settles fails with `timeout`, shown as "No response from `<mod>`".

---

## 6. Resolved design questions

The design doc left three open questions. What shipped:

**6.1 Platform state granularity: ONE document per registry, not per-mod.** The doc leaned per-mod (`osfui/settings/<mod>`) to bound push sizes; `osfui/settings`, `osfui/views` and `osfui/diagnostics` each ship as one whole-registry document. Two reasons: (1) the state protocol has no key-discovery primitive — `state.on(key, fn)` subscribes to a key you already name, so per-mod keys would need either a discovery request (re-inventing the subscribe-on-read wart 2.0 deleted) or an index key kept consistent with N siblings; (2) the primary consumer, the Mods surface, needs the whole registry anyway (every card, search, the vanilla-key conflict table, load errors). The size worry didn't materialise: the registry republishes on *shape* changes, not value commits — an individual commit is a `settings.changed` event carrying one value.

`osfui/i18n` is the deliberate exception to one-document-per-registry: one key whose **value is computed per document** (a view's catalog is its owning mod's), built per view id in `Runtime::PublishPlatformState`. Still one key with one shape; just not a broadcast.

**6.2 `SetViewState` takes JSON text at the C ABI; typed sugar lives in `OSFUI_JSON.h`.** The method is `bool SetViewState(const char*, const char*, const char*)` — text only, because `const char*` is the only shape that survives the vtable contract and no plugin should have to agree with the host on an `nlohmann` version. `sdk/OSFUI_JSON.h` adds a `SetViewState(modId, key, const Json&)` overload plus a `template <class T>` sugar, both `noexcept`, both returning `false` on a serialization throw. Header-only, opt-in, nothing but text crosses the DLL boundary.

**6.3 The retained-state cache moved out of `PapyrusApi` into the runtime.** In 1.5 the `mod\nkey` cache lived in `src/api/PapyrusApi.cpp`, because Papyrus was the only backend with state. Once `SetViewState` gave the native ABI the same verb, keeping it there meant either the ABI writing into the Papyrus module or two caches with two replay rules. It's now `src/runtime/ViewStateStore.{h,cpp}`, owned by `Runtime` and shared by both backends, with a per-entry `sessionScoped` flag rather than one store-wide policy (Papyrus values must not cross a game load; native values must). Consequences: `Papyrus::ReplayViewState` **removed** (`Runtime::OnViewGreeted` replays for both backends from the one store); `Papyrus::TakeSessionReset()` **added** so the runtime learns a load happened and calls `ViewStateStore::ClearSessionScoped()`; `DrainViewPushes` split into `DrainViewState` and `DrainViewEvents`.

**6.4 Still open: `osfui:trace` from the dev harness.** Whether the trace flag should also be settable per-view from the harness, mirroring `openDevTools`, shipped **unimplemented**. The flag is `localStorage["osfui:trace"]`, read once at helper load (`frontend/src/shared-kit/osfui.js`); there's no pipe command for it. A second control path for a debug toggle is the aliasing 2.0 exists to remove. Revisit only if setting it on a view you can't open DevTools on turns out to matter.

---

## 7. Deviations from the design doc as written

- **7.1 `osfui/debug.error` shipped as the `osfui.debug.error` EVENT.** A slash denotes a view id (`<mod>/<view>`) and the state `<mod>/<key>` separator, so a slashed name is ambiguous exactly where it matters. It shipped as a dotted event name in the platform (`osfui.*`) namespace, delivered by `MessageBridge::Surface` → `Runtime::OnProtocolMisuse` → `Emit(view, "osfui.debug.error", …)`, printed by the helper's `deliverEvent` special case with the usual `[osfui]` prefix. Naming it an event also settles what it is: one-shot, never replayed, never cached.
- **7.2 The fixed-target shell verbs shipped as requests, not commands.** The doc grouped `osfui.openModPage` and `osfui.openLogFolder` with the commands; both can fail for reasons the page can't predict and the player can act on (`shell-failed`, `no-log-folder`), and the doc's own rule is that wanting a remote outcome makes it a request. The security property that motivated grouping them — the target is a compile-time constant or natively derived and the payload can't steer the shell — is unaffected by endpoint kind. `close`, `log`, `view.ready`, `osfui.gamepadRaw` and `osfui.handleBack` did ship as commands.
- **7.4 `hud.show` / `hud.hide` and `osfui.textFocus` were deleted, not migrated.** The first two were registered to the *same handler lambdas* as `menu.open`/`menu.close`; "one dialect, no aliases" is a design principle. `osfui.textFocus` was a registered no-op kept only so a view asserting text focus before session focus wouldn't trip `unknown-command`; since an unknown send is now a dev-only debug event it bought nothing. (The WebView2 focus-on-demand mechanism it fronted is native and unaffected.)
- **7.5 Smaller drifts.** The removed `ReplayViewState` machinery moved to `src/runtime/ViewStateStore.{h,cpp}` rather than staying in `PapyrusApi` (§6.3). The doc's symmetry grid labelled the C ABI row "`RegisterCommand` (command handler)"; that's still the spelling, but the shipped header documents it as a **send** endpoint — 2.0 renamed the *host* registry (`MessageBridge::RegisterSend` / `RegisterRequest`, `IUiModule::RegisterCommands` → `RegisterEndpoints`) while keeping the C ABI method names.

---

## 8. Sequencing

There is no partial migration. The plugin DLL, `OSFUI.pex`, the shipped `views/shared/osfui.js` and `sdk/*` move together in one release, and a mod's artifacts move with them.

For a mod spanning all three backends:

1. **Bump `targetVersion` to `"2.0.0"`** in every view manifest and settings schema first. Until you do, System Health tells the player your view is legacy — what you want while you work, and what you must clear before you ship.
2. **Papyrus next**, because it's the slowest loop. Replace `PushToView` / `PushFormsToView` with `SetView*` (state) or `SendViewEvent` (happenings), collapse the `RegisterForViewActions*` family into `ListenForViewActions{,Static}` + `OnOSFUIViewAction(string, string[])`, and **delete** the page-`ready`-then-re-push handshake on both sides. Recompile against the shipped `data/Scripts/Source/OSFUI.psc` and redeploy the `.pex`.
3. **Native plugin**: 1.x binaries keep working. Recompile against `sdk/OSFUI_API.h` only to adopt `SetViewState` or current conveniences. New answer-bearing endpoints and settings-schema actions should use `RegisterRequest`; convert re-push-on-reload code to `SetViewState`.
4. **Views last**, once the data they consume already arrives correctly. Do the renames from §1.1, then the `request()` payload sweep from §1.2 — that one is manual.
5. **Verify with `localStorage["osfui:trace"] = "1"` and a reload.** Every replayed `state` envelope is visible at document boot, so a blank panel is answered immediately: the key arrives (view bug) or it doesn't (backend bug).

Carried over untouched: settings schema files, saved player values, localization catalogs, view manifests (no field changed — several changed *meaning*, because the bridge a manifest opts into is a different one), and your view's HTML/CSS.

Repository note: `data/OSFUI/views/` is **generated build output** that is committed. Edit `frontend/src/` and rebuild; never hand-edit the generated copy.

---

## 9. Test matrix

Run `bash tests/native/run.sh` (exit code = failing checks) and `npm --prefix frontend run verify` (typecheck + build + vitest). The CLI packages have their own `npm test`.

| Guarantee | Suite |
|---|---|
| Page-initiated handshake is the only boot path; hello precedes everything | `frontend/test/protocol.envelope.test.ts`, `frontend/test/handoff.test.tsx`, `tests/native/bridge_api_tests.cpp` |
| Envelope grammar: routing beside the payload, id required on request / forbidden on send, 64-char id cap, strict `invalid-request` | `frontend/test/protocol.envelope.test.ts` |
| **`request()` resolves the payload, not the envelope** | `frontend/test/protocol.errors.test.ts` |
| Typed error contract: `code` is `""` never `undefined`, message fallback chain, payload absent on local failures | `frontend/test/protocol.errors.test.ts` |
| Every failure reaches the page console with `[osfui]` | `frontend/test/protocol.errors.test.ts` |
| State: replay-on-subscribe, latest-wins, case-insensitive keys, `null` is a value, throwing handler isolated | `frontend/test/protocol.parse.test.ts` |
| Events and replies never cross channels; a request settles exactly once; correlation by id alone | `frontend/test/protocol.pluginack.test.ts` |
| `send()` is one-way; native `RegisterCommand` preserves ABI 1.x request-ID injection and auto-ack | `frontend/test/protocol.pluginack.test.ts`, `tests/native/bridge_api_tests.cpp` |
| A request endpoint that never settles answers `internal` | `frontend/test/protocol.pluginack.test.ts`, `tests/native/bridge_api_tests.cpp` |
| 1.x envelopes are ignored rather than mis-parsed | `frontend/test/protocol.parse.test.ts` |
| `osfui.debug.error` arrives as an event and is printed | `frontend/test/protocol.parse.test.ts` |
| `osfui:trace` logs both directions, only when the flag is exactly `"1"` | `frontend/test/protocol.parse.test.ts` |
| The four verbs behave as documented from an author's seat | `frontend/test/bridge.author-api.test.ts` |
| The null bridge degrades every member instead of throwing | `frontend/test/bridge.nullbridge.test.ts` |
| Removed members cannot be *called* from first-party code | Not a runtime test — enforced at typecheck against `sdk/osfui.d.ts` + `frontend/src/lib/bridge.ts` during `npm --prefix frontend run verify`. Third-party views get the runtime `TypeError`. |
| Registries arrive as state on a greeting, no read roundtrip; a late document boots from one greeting | `tests/native/settings_module_tests.cpp`, `frontend/test/settings.handshake.test.tsx` |
| `settings.set` settlement shape; a mutation sent as a `send` executes nothing | `tests/native/settings_module_tests.cpp` |
| `settings.reset` republishes state once, no per-key event spam | `tests/native/settings_module_tests.cpp` |
| `settings.captureKey` arms in machine time and commits from the `settings.captured` event | `frontend/test/keybinds.navigation.test.tsx` |
| Papyrus `SetView*` retention, replay to a fresh document, 64-key cap, form `null` slots | `tests/native/papyrus_action_tests.cpp`, `tests/native/papyrus_form_tests.cpp` |
| `SendViewEvent` is one-shot: never retained, never replayed | `tests/native/papyrus_action_tests.cpp` |
| Papyrus state is session-scoped and dropped on game load; native state is not | `tests/native/papyrus_action_tests.cpp`, `tests/native/papyrus_form_tests.cpp` |
| ABI `SetViewState` validation, queue cap, retained-not-session-scoped delivery | `tests/native/bridge_api_tests.cpp` |
| ABI 1.8 constants, additive feature gate, command compatibility | `tests/native/bridge_api_tests.cpp` |
| Different-major diagnostic ledger: dedupe by module, bounded, drained once | `tests/native/bridge_api_tests.cpp` |
| Compat card lifecycle: one source, swept by `ResolveMissing`, no occurrence churn | `tests/native/runtime_diagnostics_tests.cpp`, `tests/native/diagnostics_tests.cpp` |
| Diagnostics registry as state, including the greeting replay bypassing the content dedupe | `tests/native/diagnostics_tests.cpp` |
| Harness mock speaks the same protocol as the shipped helper, end to end | `frontend/test/devmock.mockbridge.test.ts` |
| Built views load the shipped helper and pass the output gates | `frontend/test/build.output.test.ts` |
| `targetVersion` comparison feeding the "needs update" badge | `frontend/test/version.test.ts` |
| The dev harness's traffic inspector reads 2.0 envelopes by endpoint rather than by envelope | `packages/cli/test/traffic-model.test.mjs` |
| Scaffolding refuses malformed input and yields legal ids (it does **not** compile the emitted templates) | `packages/create-osfui/test/scaffold.test.mjs` |

**Known gap.** `packages/cli/test/mock-runtime.test.mjs` wasn't migrated with `packages/cli/src/browser/mock-runtime.js`. The scenario engine's signature changed from `(command, payload, requestId, { reply, report })` to `(kind, name, payload, io)` with `io.resolve` / `io.reject` / `io.surface`, and the suite still drives the 1.x shape (including an `i18n.get` case for an endpoint that no longer exists). Fix it before trusting the CLI mock's coverage.

---

## 10. See also

- [mod-api-2.0-design.md](mod-api-2.0-design.md) — why the API is shaped this way, and what each 1.x alias papered over.
- [authoring-views.md](authoring-views.md) §3 — the full bridge reference.
- [authoring-dynamic-data.md](authoring-dynamic-data.md) — state vs. events, worked examples for both backends.
- [native-plugin-api.md](native-plugin-api.md) §1 — the 2.0 break from the plugin author's side.
- [authoring-settings.md](authoring-settings.md) §8 — consuming settings from each backend.
- `sdk/osfui.d.ts` — the typed contract (`PlatformSend`, `PlatformRequest`, `PlatformState`, `PlatformEvents`).
- `sdk/OSFUI_API.h`, `sdk/OSFUI_JSON.h` — the C ABI and its optional JSON facade.
- [logging.md](logging.md) — "Author-caused failures go to the page".
