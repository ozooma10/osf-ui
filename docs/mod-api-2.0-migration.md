# OSF UI mod API 2.0 — migration

This is the document to read when your mod stopped working after updating OSF
UI. It is the mechanics half of [mod-api-2.0-design.md](mod-api-2.0-design.md),
which explains *why* the API is shaped this way; everything below describes what
was actually implemented, against the source, with file paths you can check.

**2.0 is a hard break.** There is no compatibility shim, no deprecation window,
and no mixed mode: one OSF UI is installed and it speaks one protocol. A view or
plugin written for 1.x has to be edited, not merely re-zipped.

| Artifact | Breaks? | How you find out |
|---|---|---|
| View (`views/<modId>/<viewName>/`) | **Yes** | Every removed helper member throws `not a function`; every removed endpoint rejects or is dropped-and-surfaced. A manifest `targetVersion` below `2.0` also raises a `compat.legacy-view` card in System Health naming the view. |
| Native SFSE plugin (`sdk/OSFUI_API.h`) | **Yes** | `OSFUI_RequestBridge` returns `nullptr` (ABI major mismatch); a `compat.legacy-api` card names your DLL. |
| Papyrus script | **Partly** | Six removed natives (`PushToView`, `PushFormsToView`, `RegisterForViewActions{,Static,Args,ArgsStatic}`). Calls to them fail to resolve in the VM and error in `Papyrus.0.log`. Everything else keeps its name. |
| Settings schema (`settings/<modId>.json`) | **No**, except `action` rows | Declarative data, executes nothing. The one exception: an `action` row's `command` is now a **request** endpoint, so a plugin serving it with `RegisterCommand` makes the button report `wrong-endpoint-kind`. |
| Player values, localization catalogs | **No** | Same files, same format, carried over untouched. |

---

## 1. View authors — the JS helper

`frontend/src/shared-kit/osfui.js` (shipped to
`SFSE/Plugins/OSFUI/views/shared/osfui.js`; typed in `sdk/osfui.d.ts`).

The whole surface is four verbs — `send`, `request`, `on`, `state` — plus three
sugar namespaces (`papyrus`, `i18n`, `theme`) that add ergonomics and never new
semantics. Every 1.x alias is gone rather than deprecated.

### 1.1 Member-by-member

| 1.x member | 2.0 replacement | Break |
|---|---|---|
| `osfui.available()` | `osfui.available` — a **property**, not a call | **LOUD** — `TypeError: osfui.available is not a function` |
| `osfui.ready` (never rejected) | `osfui.ready` — same promise, now **rejects** `no-bridge` in a plain browser instead of hanging | Behavior change; loud in the standalone case (unhandled rejection at your `await`) |
| `osfui.send(command, fields)` | `osfui.send(name, payload)` | Signature-compatible, **wire changed** — see §2. Name and payload no longer merge. |
| `osfui.emit(command, fields)` | `osfui.send(name, payload)` | **LOUD** — `not a function` |
| `osfui.call(command, fields, opts)` | `osfui.request(name, payload, opts)` | **LOUD** — `not a function` |
| `osfui.request(...)` resolving the **envelope** `{ type, payload, requestId }` | `osfui.request(...)` resolving the reply **payload** | **SILENT** — see §1.2 |
| `osfui.action(name, ...args)` | `osfui.papyrus.send(name, ...args)` | **LOUD** — `not a function` |
| `osfui.papyrus.action(name, ...args)` | `osfui.papyrus.send(name, ...args)` | **LOUD** — `not a function` |
| `osfui.papyrus.request(name, ...args)` | unchanged (still unwraps to the script's value) | none |
| `osfui.viewReady()` | `osfui.markReady()` | **LOUD** — `not a function` |
| `osfui.on(type, fn)` | `osfui.on(event, fn)` — **events only**; replies never fire handlers | Name survives; see §1.3 |
| `osfui.data.on(key, fn)` | `osfui.state.on("<mod>/<key>", fn)` | **LOUD** — `osfui.data` is `undefined`, so `.on` throws |
| `osfui.data.get(key)` | `osfui.state.get("<mod>/<key>")` | **LOUD** — same |
| `osfui.t(address, english, vars)` | `osfui.i18n.t(...)` | **LOUD** — `not a function` |
| `osfui.localize(root)` | `osfui.i18n.localize(root)` | **LOUD** — `not a function` |
| `osfui.locale()` | `osfui.i18n.locale` — a **property** | **LOUD** — `not a function` |
| `osfui.i18nReady` | `osfui.i18n.ready` | **LOUD** at `.then(...)`; see the footnote in §1.3 |
| `osfui.applyAccent(el, hex)` | `osfui.theme.applyAccent(el, hex)` | **LOUD** — `not a function` |
| `osfui.onMessage` | still helper-owned; do not assign it | unchanged |
| — | `osfui.state.on/get` (new fourth verb) | — |
| — | `osfui.markReady()` (renamed `viewReady`) | — |
| — | `localStorage["osfui:trace"] = "1"` envelope tracing | — |

The typed façade the built-in views use — `frontend/src/lib/bridge.ts` — mirrors
this exactly: `available()`, `send()`, `request()`, `on()`, `onAny()`,
`state(key, fn)`, `peek(key)`, `markReady()`, `ready()`, `i18nReady()`,
`locale()`, `t()`, `applyAccent()`, `papyrusSend()`, `papyrusRequest()`.
`emit()`, `call()` and `viewReady()` are gone from it too.

### 1.2 The one silent break

> **`osfui.request()` now resolves the reply *payload*, not the whole
> envelope.** In 1.x it resolved `{ type, payload, requestId }` and you reached
> your data at `reply.payload.x`; `osfui.call()` was the variant that unwrapped
> for you. 2.0 has only the unwrapped form. Nothing throws, nothing logs,
> nothing is dropped — `reply.payload` is simply `undefined`, and every field
> you read off it is `undefined` too. **Grep every `request(` call site by hand
> and delete one `.payload` hop.** This is the only change in the entire 2.0
> API that gives you no signal at all.

```js
// 1.x
const reply = await osfui.request("game.get");
render(reply.payload.calendar);

// 2.0
const data = await osfui.request("game.get");
render(data.calendar);
```

Everything else on the removed list fails at the point of use: a removed member
throws `TypeError`, a removed *request* endpoint rejects with a typed `code`
that the helper also prints to the page console with an `[osfui]` prefix, and a
removed *send* endpoint is dropped by the host and surfaced — as an
`osfui.debug.error` event in devMode (printed to the same console), and as a
`view.protocol-misuse` health card after 10 offences in release builds
(`src/runtime/Runtime.cpp`, `Runtime::OnProtocolMisuse`, threshold
`kMisuseThreshold = 10`, diagnostics source `"views"`). Dropping is correct;
dropping silently is not.

### 1.3 Two quieter degradations worth grepping for

Neither is a contract break in the sense above — both are reachable only through
names that still exist — but both change what your handler receives.

- **`on()` handlers get one argument now.** 1.x called
  `fn(message.payload || {}, message)`; 2.0 calls `fn(payload)`. If you
  destructured the second parameter, it is `undefined`. For platform events this
  is moot (the 1.x names are gone anyway); it matters for mod-defined
  `<mod>.<name>` events fed by a plugin's `SendToWeb`.
- **Request replies no longer fan out to `on()` subscribers.** 1.x dispatched a
  correlated reply to `on()` *and* settled the promise, which is how one render
  path could consume `settings.data` whether it had asked or not. 2.0 keeps the
  channels strictly separate: a reply settles exactly one promise and reaches no
  event handler. The 1.x pattern is obsolete anyway — the registries it existed
  for are now state keys that replay to you unasked.
- *Footnote on `osfui.i18nReady`:* the member is gone, so `osfui.i18nReady.then(…)`
  throws (loud), but `await osfui.i18nReady` resolves `undefined` immediately
  rather than throwing. In practice nothing renders wrong: the helper localizes
  the document itself when the `osfui/i18n` state key arrives
  (`deliverState()` → `localize(document)`), so `data-i18n` markup is applied
  regardless. Any *manual* `osfui.t(...)` call in the same file throws.

### 1.4 Code you should delete, not port

A correct 2.0 view has **zero lifecycle code**. If you find any of these, the
replacement is deletion:

- "On ready, re-request my data." State is replayed to every fresh document
  after `ready` and before the first event. Subscribe once, at module scope.
- A page-level `ready` / `hello` action fired at your own Papyrus script so it
  re-pushes. Gone on both sides — see §4.
- Any handshake that re-asserts subscriptions after F5. `state.on()` replays the
  current value synchronously on subscribe.
- Re-asserting `osfui.gamepadRaw` / `osfui.handleBack` from a *reload handler*.
  There is no reload handler; assert them from ordinary setup code, which runs
  on every document. (The runtime drops both grants on every greeting —
  `Runtime::OnViewGreeted` clears `_gamepadRawViews` and `_backOwnerViews`.)

---

## 2. View authors — the wire

Relevant only if you talk to `postMessage`/`onMessage` directly instead of
through the helper. Source of truth: `src/runtime/MessageBridge.{h,cpp}`.

**1.x, web → native** — one shape, routing *inside* the payload:

```json
{ "type": "ui.command", "requestId": "q1",
  "payload": { "command": "settings.set", "mod": "acme.mymod", "key": "x", "value": 1 } }
```

**2.0, web → native** — routing metadata *beside* an opaque payload, so a
payload field can never override routing:

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

`RuntimeInfo` = `{ game, plugin, version, bridgeVersion, view, mod }`. `view` is
this document's own qualified id and `mod` its owning mod — the default scope
for an unqualified state key.

Envelope rules that changed from lenient to strict:

- A request `id` must be a non-empty string of at most **64 characters**. 1.x
  demoted a malformed or oversized id to fire-and-forget; 2.0 answers a hard
  `invalid-request`, because silent demotion turned a client bug into a request
  that never settles (`kMaxRequestIdLength`, `MessageBridge.cpp`).
- An `id` on a `send` is `invalid-request`.
- A non-object `payload` is `invalid-request`; absent or `null` is accepted and
  becomes `{}`.
- Endpoint kind is enforced structurally: `request()` naming a send endpoint
  rejects `wrong-endpoint-kind`; `send()` naming a request endpoint is dropped
  and surfaced.

### The handshake is page-initiated, and it is the only boot path

```
document loads
  └─ helper sends { kind:"send", name:"osfui.hello" }
       └─ bridge sends `ready`
            └─ hello hook replays state (platform keys + this mod's retained state)
                 └─ the view's event gate opens and queued events flush
```

First open, F5, dev hot-reload and crash-recovery reload are literally the same
sequence. Consequences you can rely on:

- `ready` precedes all state for that document; all replayed state precedes the
  first event.
- Events emitted before a document greets are **queued** per view, bounded at
  **64, drop-oldest** (`kMaxQueuedEventsPerView`). This is what preserves the
  native ABI's message-before-first-paint guarantee now that the page starts the
  conversation.
- State published to a view that has not greeted is **dropped**, deliberately —
  the replay covers it, so there is nothing to queue and no ordering to get
  wrong.
- A second `hello` from the same view id means a new document: the queue is
  cleared (replaying one-shot events into a fresh page would re-fire their
  effects) and the full state replay runs again.

Error codes a request can reject with: `no-bridge` (local), `timeout` (client
timer, default 10 s; `timeoutMs: 0` disables **only** the client timer),
`no-response` (backend missed the host-side 30 s deadline, `kRequestDeadline`),
`wrong-endpoint-kind`, `unknown-endpoint`, `invalid-request`,
`request-capacity` (64 concurrent deferred requests per view), `internal` (a
request endpoint returned without settling) — plus whatever code the handler
itself rejected with.

---

## 3. Endpoint reclassification

The four-verb model dissolves 1.x's subscribe-on-read wart: `settings.get`,
`views.get`, `i18n.get` and `diagnostics.get` were requests whose real job was
to *subscribe* you to future pushes. Reads-with-replay are exactly what state
is, so those four registries became state keys and the requests were deleted.

### 3.1 State keys — `osfui.state.on(key, fn)`

| Key | Replaces | Value |
|---|---|---|
| `osfui/settings` | `settings.get` → `settings.data` | The whole settings registry (`SettingsData`) |
| `osfui/views` | `views.get` → `views.data` | Every discovered surface with live open/focus/load state |
| `osfui/diagnostics` | `diagnostics.get` → `diagnostics.data` | The System Health snapshot |
| `osfui/i18n` | `i18n.get` → `i18n.data` | `{ mod, locale, strings }` — **computed per view**, the owning mod's catalog |
| `osfui/handoff` | the `handoff.state` push | (platform-private) first-load handoff state |

Plus every mod key a backend publishes: Papyrus `SetView*` and the ABI's
`SetViewState` both land under `"<yourModId>/<key>"`.

### 3.2 Events — `osfui.on(name, fn)`

`settings.changed`, `settings.persisted`, `settings.captured`, `ui.hotkey`,
`ui.visibility`, `ui.gamepad`, `osfui.debug.error` (devMode only), and every mod
event `"<mod>.<name>"` raised by Papyrus `SendViewEvent` or the ABI's
`SendToWeb`.

The first six names are unchanged from 1.x. What changed is that they are now
*only* events — nothing else arrives on this channel.

### 3.3 Sends — `osfui.send(name, payload)`

`osfui.hello`, `close`, `setVisible`, `view.ready`, `log`, `osfui.gamepadRaw`,
`osfui.handleBack`, `osfui.handoffRetry` (platform-private), `papyrus.send`.

### 3.4 Requests — `osfui.request(name, payload)`

`menu.open`, `menu.close`, `setViewHidden`, `ping`, `game.get`, `settings.set`,
`settings.reset`, `settings.captureKey`, `osfui.openModPage`,
`osfui.openLogFolder`, `osfui.setViewAutoStart`, `osfui.openReportIssue`,
`diagnostics.reportStatus`, `diagnostics.submitReport`, `papyrus.request`.

**Kind changes to watch for** — these names survived but moved from command to
request, so a 1.x `osfui.send('menu.open', …)` is now dropped and surfaced
rather than executed:

| Endpoint | 1.x | 2.0 |
|---|---|---|
| `menu.open` / `menu.close` | command | request, resolves `{}`, rejects `unknown-view` |
| `setViewHidden` | command | request, resolves `{}` |
| `osfui.openModPage` | command | request, rejects `shell-failed` |
| `osfui.openLogFolder` | command | request, rejects `no-log-folder` \| `shell-failed` |
| `osfui.openReportIssue` | command | request, rejects `forbidden` \| `invalid-issue` \| `shell-failed` |
| `ui.papyrusRequest` | request | renamed `papyrus.request` |
| `ui.action` | command | renamed `papyrus.send` |

**Settlement-shape changes:**

- `settings.set` **rejects** on failure (`forbidden`, `unknown-setting`,
  `read-only`, `invalid-value`) instead of resolving `settings.ack { ok:false }`
  that the caller had to remember to inspect, and resolves
  `{ mod, key, value }` where `value` is the **post-clamp committed** value — so
  clamped and accepted are distinguishable without a re-read
  (`src/runtime/SettingsModule.cpp`).
- `settings.reset` resolves `{}`. The refreshed registry reaches every view —
  including the caller — through the `osfui/settings` state key; carrying the
  document in the reply too would make the caller's copy arrive by a different
  route than everyone else's.
- `settings.captureKey` settles in **machine time**: it resolves
  `{ armed: true, mod, key }` or rejects `capture-busy` / `forbidden` /
  `not-rebindable`. The captured key (or the cancellation) arrives afterwards as
  the `settings.captured` **event**, however long the player takes. There is no
  `timeoutMs: 0` usage anywhere any more — a request that waits on a human is
  the wrong shape and fights the client timer.
- `ping` resolves `{}` (was a `runtime.pong` message); `game.get` resolves
  `GameData` directly (was a `game.data` message).

### 3.5 Deleted endpoints

| Deleted | Why / what to use |
|---|---|
| `hud.show`, `hud.hide` | Pure aliases of `menu.open`/`menu.close` — they were registered to the *same handler lambdas* in 1.x. Deleted, not migrated: the design's "one dialect, no aliases" rule. Use `menu.open` / `menu.close` (as **requests**). |
| `osfui.textFocus` | Was registered as a **no-op** in 1.x, purely so a pre-session-focus view would not trip `unknown-command`. An unknown send is now a dev-only debug event, so the placeholder bought nothing. Deleted; nothing replaces it. |
| `settings.get`, `views.get`, `i18n.get`, `diagnostics.get` | The four subscribe-on-read requests. Use the state keys in §3.1. |
| `ui.action` | Renamed `papyrus.send` (helper: `osfui.papyrus.send`). |
| `ui.papyrusRequest` | Renamed `papyrus.request` (helper: `osfui.papyrus.request`). |

### 3.6 Deleted native → web message types

Every one of these is gone; the `kind` field replaces the whole `type` taxonomy.

`runtime.ready` (now `kind:"ready"`), `ui.result`, `ui.error` (now
`kind:"reply"` / `kind:"error"`), `settings.ack`, `settings.data`, `views.data`,
`i18n.data`, `diagnostics.data`, `diagnostics.reportResult` (now the deferred
reply to `diagnostics.submitReport`), `data.push`, `data.state` (now
`kind:"state"`), `papyrus.result`, `runtime.pong`, `game.data`, `handoff.state`.

---

## 4. Papyrus authors

`data/Scripts/Source/OSFUI.psc`, natives in `src/api/PapyrusApi.{h,cpp}`.

Papyrus keeps its 1.5 names on purpose: renaming `ListenForViewActions` /
`OnOSFUIViewAction` would churn exactly the mods that already migrated, in the
one language where migrating means recompiling `.pex` files.

| 1.x | 2.0 | Break |
|---|---|---|
| `PushToView(mod, key, values)` | `SetViewString`s/`SetViewInt`s/… for state, `SendViewEvent` for happenings | **LOUD** — the native no longer exists or binds; the call fails to resolve and errors in `Papyrus.0.log`, unwinding that stack |
| `PushFormsToView(mod, key, forms)` | `SetViewForms(mod, key, forms)` | **LOUD** — same |
| `RegisterForViewActions(receiver, fn, mod)` | `ListenForViewActions(receiver, mod)` → `OnOSFUIViewAction(string, string[])` | **LOUD** — same |
| `RegisterForViewActionsStatic(script, fn, mod)` | `ListenForViewActionsStatic(script, mod)` | **LOUD** — same |
| `RegisterForViewActionsArgs(...)` | `ListenForViewActions(...)` | **LOUD** — same |
| `RegisterForViewActionsArgsStatic(...)` | `ListenForViewActionsStatic(...)` | **LOUD** — same |
| — | **`SendViewEvent(mod, name, args)`** (new) | Emits event `"<mod>.<name>"` with payload `{ args }`; never cached, never replayed |
| `SetViewBool/Int/Float/String/Bools/Ints/Floats/Strings/Forms` | unchanged | none — but the wire shape is now `kind:"state"` and the view consumes it with `osfui.state.on()` |
| `ListenForViewRequests{,Static}`, `ReplyView*`, `RejectViewRequest` | unchanged | none |
| `GetFormById`, `GetFormsById`, `Unregister`, `OpenMenu`, `CloseMenu` | unchanged | none |
| `RegisterForSettingChanges{,Static}`, `RegisterForHotkey{,Static}`, `Get*`/`Set*`/`Reset` | unchanged | none |

Notes:

- `PushToView` was **transient like an event but shaped like state**. That is
  precisely why every mod using it needed a page-level `ready` action and a
  matching re-push in the script. Splitting the concepts is what let that whole
  convention be deleted rather than documented. Delete both halves.
- Papyrus `SetView*` state stays **session-scoped**: values can hold form
  identities, which do not survive a game load. `Papyrus::TakeSessionReset()`
  reports the load and the runtime drops those entries
  (`ViewStateStore::ClearSessionScoped`). Republish after a load, exactly as in
  1.5. Native `SetViewState` is *not* session-scoped.
- The retained-state mechanics carry over: latest-wins per `mod`+`key`,
  case-insensitive keys (Papyrus `BSFixedString` interning hands back the
  first-seen casing, so your script's spelling does not survive the trip),
  at most **64 keys per mod**, `None` form slots preserved as JSON `null` so a
  parallel values key stays index-aligned.
- The C++-side names moved with the concept: `ViewPush` became `ViewState`
  (`{ mod, key, value }`) plus `ViewEvent` (`{ mod, name, args }`);
  `DrainViewPushes` became `DrainViewState` + `DrainViewEvents`; and
  `ReplayViewState` was removed entirely because the runtime owns the cache now
  (§5.3).

---

## 5. Native plugin authors

`sdk/OSFUI_API.h` (+ the optional `sdk/OSFUI_JSON.h` facade), host side in
`src/api/BridgeApi.{h,cpp}` and `src/api/Exports.cpp`.

### 5.1 The hard break

```cpp
inline constexpr std::uint32_t kBridgeAPIVersion = (2u << 16) | 0u;
```

`OSFUI_RequestBridge` compares only the **major**. A plugin built against any
1.x header gets `nullptr`, `Client::Init()` returns `false`, and every
subsequent `Client` call degrades to `false` / `0` / no-op. Your plugin keeps
running; its UI simply never appears.

There is no compatibility dispatcher, and that is a decision rather than an
oversight. Serving a 1.x caller means keeping the auto-ack and the injected
`requestId` alive *inside* the 2.0 host — the exact bookkeeping 2.0 exists to
delete — forever, for plugins that must be recompiled anyway because their view
helper members and Papyrus registrations moved in the same release. There is
also a mechanical reason it could not have been avoided: `SetViewState` was
inserted **immediately after `SendToWeb`**, at vtable slot 7, not appended at
the end. Every method after it shifted index, so a 1.x plugin calling
`SetReadyCallback` through a 2.0 vtable would have called `SetViewState`
instead. The major bump is what makes that safe.

### 5.2 What a 1.x plugin experiences, and the `compat.legacy-api` card

The refusal is not silent. `src/api/Exports.cpp` takes `_ReturnAddress()` — the
caller reached the export through its own `GetProcAddress` call site, so the
return address is inside its module — and resolves it to a bare DLL name
(`Platform::ModuleNameForAddress`). That is recorded with
`BridgeApi::NoteLegacyApiCaller(module, major, minor)`.

The refusal happens during SFSE load, long before diagnostics exist, so the
ledger is drained later by `RuntimeDiagnostics::SyncCompatibility`, which raises
one **`compat.legacy-api`** card per module in System Health, naming the file the
player has to update, with the caller's ABI version as context. The ledger is
deduplicated by module (a plugin retrying every load screen still gets one card)
and bounded at 32 entries so a load order full of stale plugins cannot grow it.

A view whose manifest declares a `targetVersion` below `2.0` gets the sibling
card **`compat.legacy-view`** (`IsPre2Target`, `src/core/Version.h`). An
*undeclared* `targetVersion` is deliberately excluded: after parsing it is
indistinguishable from "declared and unparsable", and guessing would badge every
undeclared view.

Both codes ride on `CompatibilityTarget`, which grew a `code` field defaulting
to `"compat.needs-newer-osfui"` (`src/runtime/DiagnosticsReconciler.h`). Every
compat producer **must** flow through the one `SyncCompatibility` call, because
`ResolveMissing` sweeps by `source` — two independent producers writing
`source: "compat"` would each resolve the other's issues on every pass.

### 5.3 Source changes

| 1.x | 2.0 | Break |
|---|---|---|
| command payloads carried an injected `"requestId"` | nothing is injected — the payload is the caller's object, verbatim | Compile-clean, behavior change. Your correlation code has nothing to correlate. |
| the host auto-acked `ui.result { ok:true }` when your handler returned | nothing is sent; a registered command **is** a send endpoint | A view that `await`ed your command now gets `wrong-endpoint-kind` |
| a command that wanted to answer replied out-of-band with the injected id | `RegisterRequest` + `Request::Respond` / `Request::Reject` (unchanged since ABI 1.7) | — |
| `SendToWeb(viewId, type, payloadJson)` | unchanged signature; now encodes `{ kind:"event", name: type, payload }` | none for the plugin; the view still reads it at `osfui.on(type)` |
| — | **`SetViewState(modId, key, payloadJson)`** (new, vtable slot 7) | — |
| `Feature` gates decided whether a call worked | every feature is baseline in 2.x — the major mismatch refuses the bridge outright, so nothing can be running against a 1.x host | `Feature::kViewState = 0` |
| — | `BridgeApi::TakeViewStateOps()`, `BridgeApi::TakeLegacyApiCallers()` (new drains) | — |

`SetViewState` is the systemic fix for the blank-after-F5 bug class on the
native side. 1.x state was Papyrus-only, so a plugin had to invent its own
reload handshake — listen for a view-defined hello, re-push — and every one of
those was a chance to get it wrong. Publish once and the runtime replays it to
every document of your mod, forever:

- latest-wins per key, and the value is **complete** — never a delta;
- any JSON *value* (object, array, number, …), validated synchronously;
- keys matched case-insensitively, at most 64 per mod;
- **not** session-scoped — wiping a HUD's configuration on every save load would
  be a bug. (Papyrus `SetView*` state *is*, because it can hold form
  identities.)

### 5.4 Settings-schema `action` rows now need `RegisterRequest`

An `action` row's `command` was a fire-and-forget command with an ack
convention in 1.x. In 2.0 the Mods surface fires it as a **request**
(`frontend/src/views/osfui/settings/App.tsx`, `runAction`, 5 s timeout) and
renders the settlement: `Respond` with an object — an optional string `message`
becomes a toast, `{}` is a silent success — or `Reject` with a code and
sentence, rendered as an error toast. A handler registered with
`RegisterCommand` makes the button fail with `wrong-endpoint-kind`; one that
never settles fails with `timeout`, shown as "No response from `<mod>`".

---

## 6. Resolved design questions

The design doc closed with three open questions. Two were resolved during
implementation; a third design decision (not listed there) also had to be
settled.

### 6.1 Platform state granularity: ONE document per registry, not per-mod

The design doc leaned per-mod (`osfui/settings/<mod>`) to bound push sizes. It
shipped as one document per registry: `osfui/settings` carries the entire
registry, `osfui/views` every discovered surface, `osfui/diagnostics` the whole
snapshot.

Two reasons, in order of weight:

1. **The state protocol has no key-discovery primitive.** `state.on(key, fn)`
   subscribes to a key you already name. Per-mod keys would mean a view could
   not find out *which* mods exist without either a discovery request — which
   re-invents the subscribe-on-read wart 2.0 just deleted — or a
   `osfui/settings/index` key that has to stay consistent with N sibling keys.
   The whole point of state is that subscribing *is* the read.
2. **The primary consumer needs the whole registry anyway.** The Mods surface
   renders every mod's card, search across all of them, the vanilla-key
   conflict table and the load-error list. Per-mod keys would force it to
   subscribe to N keys and reassemble a document the host already had.

The size worry it was hedging against did not materialise: the registry is
republished on *shape* changes, not on value commits — an individual commit is
a `settings.changed` event carrying one value
(`src/runtime/SettingsModule.cpp`).

`osfui/i18n` is the deliberate exception to "one document per registry": it is
one key, but its **value is computed per document** — a view's catalog is its
owning mod's, so `Runtime::PublishPlatformState` builds it per view id. That is
still one key with one shape; it just is not a broadcast.

### 6.2 `SetViewState` takes JSON text at the C ABI; typed sugar lives in `OSFUI_JSON.h`

The C ABI method is `bool SetViewState(const char* modId, const char* key,
const char* payloadJson)` — text only. `const char*` is the only shape that
survives the vtable contract, and no plugin should have to agree with the host
on an `nlohmann` version to publish a value.

The ergonomics live one layer up, exactly where `JsonRequest::Respond` already
put them: `sdk/OSFUI_JSON.h` grew a `SetViewState(modId, key, const Json&)`
overload plus a `template <class T>` sugar that constructs the `Json` for you,
both `noexcept` and both returning `false` on a serialization throw. Header-only,
opt-in, and nothing crosses the DLL boundary but text.

### 6.3 The retained-state cache moved out of `PapyrusApi` into the runtime

Not one of the doc's open questions, but the design's backend-symmetry grid
forced it. In 1.5 the `mod\nkey` cache lived inside `src/api/PapyrusApi.cpp`,
because Papyrus was the only backend with state. Once `SetViewState` gave the
native ABI the same verb, keeping the cache there would have meant either the
ABI writing into the Papyrus module or two independent caches with two replay
rules.

It is now `src/runtime/ViewStateStore.{h,cpp}`, owned by `Runtime` and shared by
both backends, which is what makes the symmetry grid actually square. The entry
carries its own `sessionScoped` flag rather than the store having one policy —
Papyrus values must not cross a game load (form identities), native values must
(a HUD's configuration). Consequences:

- `Papyrus::ReplayViewState` was **removed**; `Runtime::OnViewGreeted` does the
  replay for both backends from the one store.
- `Papyrus::TakeSessionReset()` was **added** so the runtime learns a load
  happened and calls `ViewStateStore::ClearSessionScoped()`.
- `DrainViewPushes` split into `DrainViewState` and `DrainViewEvents`.

### 6.4 Still open: `osfui:trace` from the dev harness

The third design question — should the trace flag also be settable per-view from
the dev harness, mirroring `openDevTools` — shipped **unimplemented**. The flag
is `localStorage["osfui:trace"]`, read once at helper load
(`frontend/src/shared-kit/osfui.js`). There is no pipe command for it. This is a
deliberate deferral, not an oversight: the localStorage route already works for
every view including world surfaces, and adding a second control path for a
debug toggle is exactly the kind of aliasing 2.0 exists to remove. Revisit only
if setting it on a view you cannot open DevTools on turns out to matter.

---

## 7. Deviations from the design doc as written

The design doc was written before implementation. Five things shipped
differently.

### 7.1 `osfui/debug.error` shipped as the `osfui.debug.error` EVENT

The doc named the dev-only error channel `osfui/debug.error`. A slash denotes a
**view id** (`<mod>/<view>`) throughout the system — and, in the state
namespace, the `<mod>/<key>` separator. A name with a slash in it is therefore
ambiguous in the one place it matters. It shipped as a dotted **event** name,
`osfui.debug.error`, in the platform (`osfui.*`) endpoint namespace, delivered
by `MessageBridge::Surface` → `Runtime::OnProtocolMisuse` → `Emit(view,
"osfui.debug.error", …)`, and printed by the helper's `deliverEvent` special
case with the same `[osfui]` prefix as client-detected failures. Naming it an
event also settles what it is: a one-shot happening, never replayed, never
cached — which is correct, and which the slash spelling left open.

### 7.2 The fixed-target shell verbs shipped as requests, not commands

The doc's "Failure semantics" section listed `close`, `log`, `view.ready`, the
input-mode declarations "and the fixed-target shell verbs" together as commands.
`osfui.openModPage`, `osfui.openLogFolder` and `osfui.openReportIssue` shipped
as **requests**.

The reason is the doc's own rule — *wanting a remote outcome means it is a
request* — applied honestly. All three can fail for reasons the page cannot
predict and the player can act on: the shell refuses (`shell-failed`), the
Documents folder will not resolve (`no-log-folder`), the caller is not the Mods
surface (`forbidden`), the issue number is junk (`invalid-issue`). System Health
already surfaces those failures to the player, so the information exists; making
them commands would have meant computing it and then throwing it away. The
security property that motivated grouping them — the target is a compile-time
constant or natively derived, and the payload cannot steer the shell — is
unaffected by the endpoint kind.

`close`, `log`, `view.ready`, `osfui.gamepadRaw` and `osfui.handleBack` did ship
as commands, as written.

### 7.3 `SendRuntimeReady` had three call sites, not four — and world surfaces had none

The doc cited "the four separate `SendRuntimeReady` call sites in
`src/runtime/Runtime.cpp`" as machinery the page-initiated handshake would
delete. There were **three** (at `main` — `LoadSurface`, the view-recreate path,
and the host-restart loop).

The more interesting half is the fourth that did not exist: **world surfaces
never greeted at all.** They are created in their own loop, with their own host
process and renderer, and never pass through `LoadSurface` — 1.x had to mark
them loaded by hand (`SetSurfaceLoaded(instance.viewId, true)`) precisely
because that path was bypassed. Nothing ever sent them `runtime.ready`, so
`osfui.ready` never resolved on a world surface and the boot code behind it
never ran.

The page-initiated handshake fixes this for free, with no world-surface-specific
code: `MessageBridge::HandleHello` opens `_gates[viewId]` on demand, so a
document that greets gets `ready`, its state replay and its event gate whether
or not anything called `OnViewCreated` for it. The runtime still only calls
`OnViewCreated` on the three overlay paths; a world surface simply arrives at
hello without a pre-armed gate and is handled identically. This is the clearest
evidence for the design's claim that a page-initiated handshake removes a class
of "did the greeting land?" bookkeeping rather than relocating it.

### 7.4 `hud.show` / `hud.hide` and `osfui.textFocus` were deleted, not migrated

The doc's reclassification section did not name them. All three were **deleted
outright** rather than reclassified:

- `hud.show` / `hud.hide` were registered to the *same handler lambdas* as
  `menu.open` / `menu.close` in 1.x. They were aliases in the most literal sense
  available, and "one dialect, no aliases" is a design principle, not a
  preference.
- `osfui.textFocus` was a registered **no-op**, kept alive only so a view
  asserting text focus before session focus existed would not trip
  `unknown-command`. Since an unknown send is now a dev-only debug event rather
  than a user-visible error, the placeholder bought nothing. (The WebView2
  focus-on-demand mechanism it once fronted is native and unaffected.)

### 7.5 Smaller drifts

- The doc described the removed `ReplayViewState` machinery as staying in
  `PapyrusApi`; it moved to `src/runtime/ViewStateStore.{h,cpp}` — §6.3.
- The doc's backend-symmetry grid labelled the C ABI row "`RegisterCommand`
  (command handler)". That is still the spelling, but the shipped header
  documents it unambiguously as a **send** endpoint: 2.0 renamed the *host*
  registry (`MessageBridge::RegisterSend` / `RegisterRequest`, and
  `IUiModule::RegisterCommands` → `RegisterEndpoints`) while keeping the C ABI
  method names, so no plugin has to rename a call it is already recompiling.

---

## 8. Sequencing

There is no partial migration. The plugin DLL, `OSFUI.pex`, the shipped
`views/shared/osfui.js`, and `sdk/*` all move together in one release, and a
mod's own artifacts have to move with them.

Recommended order for a mod that spans all three backends:

1. **Bump `targetVersion` to `"2.0.0"`** in every view manifest and settings
   schema *first*. Until you do, System Health tells the player your view is
   legacy — which is what you want while you work, and what you must clear
   before you ship.
2. **Papyrus next**, because it is the slowest loop. Replace `PushToView` /
   `PushFormsToView` with `SetView*` (state) or `SendViewEvent` (happenings),
   collapse the `RegisterForViewActions*` family into
   `ListenForViewActions{,Static}` + `OnOSFUIViewAction(string, string[])`, and
   **delete** the page-`ready`-then-re-push handshake on both sides. Recompile
   against the shipped `data/Scripts/Source/OSFUI.psc` and redeploy the `.pex`.
3. **Native plugin**: recompile against `sdk/OSFUI_API.h`. Grep for
   `"requestId"` in command handlers (nothing is injected any more), move any
   command that answers to `RegisterRequest`, move any command serving a
   settings-schema `action` row to `RegisterRequest`, and convert
   re-push-on-reload code to `SetViewState`.
4. **Views last**, when the data they consume already arrives correctly. Do the
   mechanical renames from §1.1, then the `request()` payload sweep from §1.2 —
   that one is manual, because nothing will tell you.
5. **Verify with `localStorage["osfui:trace"] = "1"` and a reload.** Every
   replayed `state` envelope is visible at document boot, so a blank panel is
   answered immediately: the key arrives (view bug) or it does not (backend
   bug).

What carries over untouched: settings schema files, the player's saved values,
localization catalogs, view manifests (no field changed — several fields changed
*meaning*, because the bridge a manifest opts into is a different one), and
your view's HTML/CSS.

Repository note: `data/OSFUI/views/` is **generated build output** that is
committed. Edit `frontend/src/` and rebuild; never hand-edit the generated copy.

---

## 9. Test matrix

Run: `bash tests/native/run.sh` (exit code = failing checks) and
`npm --prefix frontend run verify` (typecheck + build + vitest). The CLI
packages have their own `npm test`.

| Guarantee | Suite |
|---|---|
| Page-initiated handshake is the only boot path; hello precedes everything | `frontend/test/protocol.envelope.test.ts` ("the handshake is page-initiated"), `frontend/test/handoff.test.tsx` ("greets the host itself"), `tests/native/bridge_api_tests.cpp` ("the handshake is PAGE-INITIATED and is the only boot path") |
| Envelope grammar: routing beside the payload, id required on request / forbidden on send, 64-char id cap, strict `invalid-request` | `frontend/test/protocol.envelope.test.ts` ("host envelope validation — 2.0 REJECTS where 1.x silently demoted") |
| **`request()` resolves the payload, not the envelope** | `frontend/test/protocol.errors.test.ts` ("resolves a reply with the PAYLOAD, not the envelope"; "resolves a reply even when its payload READS like a failure") |
| Typed error contract: `code` is `""` never `undefined`, message fallback chain, payload absent on local failures | `frontend/test/protocol.errors.test.ts` (BridgeError contract blocks) |
| Every failure reaches the page console with `[osfui]` | `frontend/test/protocol.errors.test.ts` ("every failure reaches the page console with an [osfui] prefix") |
| State: replay-on-subscribe, latest-wins, case-insensitive keys, `null` is a value, throwing handler isolated | `frontend/test/protocol.parse.test.ts` (`kind:"state"` block) |
| Events and replies never cross channels; a request settles exactly once; correlation by id alone | `frontend/test/protocol.pluginack.test.ts` |
| A registered command is a send — nothing to await, nothing to ack; `wrong-endpoint-kind` both directions | `frontend/test/protocol.pluginack.test.ts` ("a command is a send"), `tests/native/bridge_api_tests.cpp` ("a registered command is a SEND endpoint", "kind enforcement") |
| A request endpoint that never settles answers `internal` | `frontend/test/protocol.pluginack.test.ts`, `tests/native/bridge_api_tests.cpp` ("request settlement") |
| 1.x envelopes are ignored rather than mis-parsed | `frontend/test/protocol.parse.test.ts` ("ignores a 1.x envelope entirely") |
| `osfui.debug.error` arrives as an event and is printed | `frontend/test/protocol.parse.test.ts` ("host-detected misuse arrives as osfui.debug.error and is PRINTED") |
| `osfui:trace` logs both directions, only when the flag is exactly `"1"` | `frontend/test/protocol.parse.test.ts` ("the osfui:trace flag") |
| The four verbs behave as documented from an author's seat (greet, correlate, unwrap, cache, isolate mods) | `frontend/test/bridge.author-api.test.ts` |
| The null bridge degrades every member instead of throwing (standalone preview / unit tests) | `frontend/test/bridge.nullbridge.test.ts` |
| Removed members cannot be *called* from first-party code | Not a runtime test: enforced at typecheck against `sdk/osfui.d.ts` + `frontend/src/lib/bridge.ts` during `npm --prefix frontend run verify`. Third-party views get the runtime `TypeError` instead. |
| Registries arrive as state on a greeting, with no read roundtrip; a late document boots from one greeting | `tests/native/settings_module_tests.cpp` ("boot: the registry arrives as STATE", "a late document boots"), `frontend/test/settings.handshake.test.tsx` ("issues no read at all — subscribing is the read") |
| `settings.set` settlement shape (post-clamp value, machine failure codes); a mutation sent as a `send` executes nothing | `tests/native/settings_module_tests.cpp` ("settlement shape", "kind enforcement: a mutation sent as a `send` executes nothing") |
| `settings.reset` republishes state once, no per-key event spam | `tests/native/settings_module_tests.cpp` ("settings.reset: ONE state republish") |
| `settings.captureKey` arms in machine time and commits from the `settings.captured` event | `frontend/test/keybinds.navigation.test.tsx` ("arms with a request and commits from the settings.captured EVENT") |
| Papyrus `SetView*` retention, replay to a fresh document, 64-key cap, form `null` slots | `tests/native/papyrus_action_tests.cpp` ("SetView* state queue / drain", "retention: what a fresh document is replayed", "ViewStateStore, directly"), `tests/native/papyrus_form_tests.cpp` |
| `SendViewEvent` is one-shot: never retained, never replayed | `tests/native/papyrus_action_tests.cpp` ("SendViewEvent: one-shot, never retained, never replayed") |
| Papyrus state is session-scoped and dropped on game load; native state is not | `tests/native/papyrus_action_tests.cpp` ("load-game teardown"), `tests/native/papyrus_form_tests.cpp` ("session scope") |
| ABI `SetViewState` validation, queue cap, retained-not-session-scoped delivery | `tests/native/bridge_api_tests.cpp` ("SetViewState (ABI 2.0): retained state, not a happening") |
| ABI 2.0 version constants and the hard break | `tests/native/bridge_api_tests.cpp` ("version constants: the 2.0 hard break") |
| `compat.legacy-api` ledger: dedupe by module, bounded, drained once | `tests/native/bridge_api_tests.cpp` ("legacy-ABI callers (the 2.0 hard break, made visible)") |
| Compat card lifecycle: one source, swept by `ResolveMissing`, no occurrence churn | `tests/native/runtime_diagnostics_tests.cpp`, `tests/native/diagnostics_tests.cpp` ("ResolveMissing sweeps one source against a recomputed set") |
| Diagnostics registry as state, including the greeting replay bypassing the content dedupe | `tests/native/diagnostics_tests.cpp` ("The registry as STATE", "REGRESSION: the hello replay must BYPASS the content dedupe") |
| Harness mock speaks the same protocol as the shipped helper, end to end | `frontend/test/devmock.mockbridge.test.ts` ("the shipped shared kit talks to it end to end") |
| Built views load the shipped helper and pass the output gates | `frontend/test/build.output.test.ts` |
| `targetVersion` comparison feeding the "needs update" badge | `frontend/test/version.test.ts` |
| The dev harness's traffic inspector reads 2.0 envelopes by endpoint rather than by envelope | `packages/cli/test/traffic-model.test.mjs` ("sends and requests read as the endpoint, not the envelope"; "state rows name the mod and key") |
| Scaffolding refuses malformed input and yields legal ids (it does **not** compile the emitted templates) | `packages/create-osfui/test/scaffold.test.mjs` |

**Known gap.** `packages/cli/test/mock-runtime.test.mjs` was not migrated with
`packages/cli/src/browser/mock-runtime.js`. The scenario engine's signature
changed from `(command, payload, requestId, { reply, report })` to
`(kind, name, payload, io)` with `io.resolve` / `io.reject` / `io.surface`, and
the suite still drives the 1.x shape (including an `i18n.get` case for an
endpoint that no longer exists). Fix that suite before trusting the CLI mock's
coverage.

---

## 10. See also

- [mod-api-2.0-design.md](mod-api-2.0-design.md) — why the API is shaped this
  way, and what each 1.x alias was papering over.
- [authoring-views.md](authoring-views.md) §3 — the full bridge reference.
- [authoring-dynamic-data.md](authoring-dynamic-data.md) — state vs. events,
  worked examples for both backends, and its own "Coming from 1.x" table.
- [native-plugin-api.md](native-plugin-api.md) §1 — "The 2.0 break", from the
  plugin author's side.
- [authoring-settings.md](authoring-settings.md) §8 — consuming settings from
  each backend.
- `sdk/osfui.d.ts` — the typed contract (`PlatformSend`, `PlatformRequest`,
  `PlatformState`, `PlatformEvents`).
- `sdk/OSFUI_API.h`, `sdk/OSFUI_JSON.h` — the C ABI and its optional JSON
  facade.
- [logging.md](logging.md) — "Author-caused failures go to the page".
