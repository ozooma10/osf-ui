# OSF UI 2.0 mod API: design

Status: IMPLEMENTED in 2.0.0 (this file is kept as the design record, not
updated to describe the code). This document describes the target
design; the migration mechanics (renames, test matrix, sequencing) live in
[the 2.0 migration plan](mod-api-2.0-migration.md) and are only referenced here.
That document is written against what was actually implemented, and records
where the shipped code deviates from this one.

## What this system is

Strip away the helper and its aliases and OSF UI is one thing: a
**reload-prone web document** talking to **three backends** — the platform
runtime (settings, views, i18n, diagnostics), an optional native SFSE plugin,
and Papyrus scripts. Every API wart in 1.x traces back to not answering two
questions consistently:

1. **Does the caller need a completion?** This produced the send/emit and
   request/call aliases, command auto-acks, the injected `requestId` in plugin
   command payloads, and the helper's "foreign ack" heuristic
   (`frontend/src/shared-kit/osfui.js`, `src/api/BridgeApi.cpp`).
2. **What happens on F5?** This produced the `data.push` vs `data.state`
   split, the "fire a `ready` action so the script re-pushes" convention, and
   the blank-after-reload bug class in consumers.

2.0 organizes the API around those two questions instead of around transport
mechanics.

## Design principles

1. **Four verbs, chosen by semantics.** Needs a completion → `request`.
   Ongoing values → `state`. One-shot happenings → `on` (events). Everything
   else → `send`. Nothing else exists.
2. **One dialect, no aliases.** Sugar namespaces (`papyrus`, `i18n`, `theme`)
   add ergonomics on top of the verbs; they never add new semantics. This is
   the rule that prevents the 1.x alias sprawl from regrowing.
3. **Reload is the common case.** Every contract must answer "what happens on
   F5?" State replays; events don't; pending requests die with the document;
   the handshake re-runs. A correctly written view has **zero lifecycle
   code** — if an author ever writes "on ready, re-request my data", the
   design has failed.
4. **Routing metadata lives outside the user payload.** The 1.x wire puts the
   command name *inside* the payload and the helper builds it with
   `Object.assign({ command }, fields)`, so a payload field can override
   routing. 2.0 envelopes carry `kind`/`name`/`id` beside an opaque payload.
5. **Errors are typed, settle exactly once, and are loud in development.**
   Every failure a mod author can cause is visible in the view's own console —
   and therefore in the F12 Chromium DevTools — not only in a log file
   (see "DevTools").
6. **Symmetry across backends.** Platform, native plugin, and Papyrus each
   express the same four endpoint kinds. Gaps in that grid are API bugs.

## The four verbs

`state` is not a data namespace hanging off the helper — it is a fourth verb
with its own delivery contract. The events/state distinction is load-bearing:
events answer "something happened", state answers "what is true now".
Replaying an event on reload is a bug (the effect re-fires); *not* replaying
state on reload is a bug (the blank HUD). No single primitive serves both,
which is why 1.x grew both `data.push` and `data.state`.

| Verb | Direction | Contract | On F5 / reload / recovery |
|---|---|---|---|
| `send` | web → backend | One-way. Returns "posted locally" boolean; never reports remote outcome. | Nothing — gone with the document |
| `request` | web → backend | Exactly one settlement: payload, typed error, or timeout. | Pending requests die with the document |
| `on` (events) | backend → web | One-shot happenings, delivered at most once. **Never replayed.** | Missed events stay missed |
| `state` | backend → web | Named values, latest-wins, complete value per key (never deltas). **Always replayed** to a fresh document, after `ready`, before any event. | Replayed automatically — this is the point |

Rule of thumb for authors: *if the backend knows when the value changes,
publish state; if only the view knows when it needs the answer, make a
request.*

## Public JavaScript surface

```ts
interface OSFUI {
  readonly available: boolean;
  readonly ready: Promise<RuntimeInfo>;        // rejects 'no-bridge' in a plain browser

  send(name: string, payload?: JsonObject): boolean;
  request<T>(name: string, payload?: JsonObject, opts?: { timeoutMs?: number }): Promise<T>;
  on<T>(event: string, fn: (payload: T) => void): () => void;

  state: {
    get<T>(key: string): T | undefined;
    on<T>(key: string, fn: (value: T) => void): () => void;  // replays current value immediately
  };

  markReady(): boolean;                        // sugar: send('view.ready')

  papyrus: {                                   // direct GLOBAL call + listener endpoints
    float(value: number): PapyrusFloatArgument;
    call(script: string, fn: string, ...args: PapyrusCallArgument[]): boolean;
    send(name: string, ...args: PapyrusArgument[]): boolean;
    request<T>(name: string, ...args: PapyrusArgument[]): Promise<T>;
  };
  i18n: {
    readonly ready: Promise<I18nInfo>;
    readonly locale: string;
    t(address: string, english: string, vars?: Record<string, string | number>): string;
    localize(root?: ParentNode): void;
  };
  theme: {
    applyAccent(element: HTMLElement, hex?: string | null): void;
  };
}
```

Notes:

- `request()` resolves with the reply **payload**; correlation envelopes and
  ids are private. (1.x `request()` resolved with the whole envelope — this is
  the one migration change that fails *silently* rather than loudly; the
  migration table flags it in bold.)
- `on()` receives only unsolicited events. Request replies never fire event
  handlers (1.x fired both).
- `state.on()` replays the current cached value synchronously on subscribe,
  then fires on every change. `state.get()` exists for imperative reads.
- `i18n.t`/`i18n.localize` are pure functions over the locale-catalog state
  key; `theme.applyAccent` never touches the wire. Neither namespace carries
  bridge semantics.
- There is no `emit`, `call`, `action`, `viewReady`, `data`, or top-level
  i18n/theme member. Removed members fail loudly (`not a function`) when 1.x
  view code runs against the 2.0 helper.

## Wire protocol

Exactly one inbound shape per verb; routing metadata beside, not inside, the
payload:

```
web → native:   { kind: "send" | "request",  name: string, id?: string, payload: {} }

native → web:   { kind: "reply" | "error",   id: string,   payload: {} | { code, message } }
                { kind: "event",             name: string, payload: {} }
                { kind: "state",             mod: string,  key: string, value: any }
                { kind: "ready",             payload: RuntimeInfo }
```

- `id` is required on `request`, forbidden on `send`. The 1.x rule that an
  oversized/malformed id demotes the message to fire-and-forget is replaced by
  a hard `invalid-request` error — silent demotion hides bugs.
- Endpoint-kind enforcement is structural: a `send` naming a request endpoint
  and a `request` naming a strict send endpoint are both kind mismatches,
  rejected uniformly (`wrong-endpoint-kind` for requests;
  dropped-and-surfaced for sends — see "Failure semantics"). The native ABI
  `RegisterCommand` boundary is the compatibility exception: it accepts both
  verbs and preserves the 1.x injected `requestId` plus auto-ack contract.
  Explicit `RegisterRequest` and every platform endpoint remain strict, and
  the helper has no foreign-ack heuristic.
- Name grammar keeps today's structural partition
  (`src/api/BridgeApi.cpp` `IsValidPluginCommand`): platform endpoints are
  undotted or `osfui.*`; mod endpoints are `<author>.<modname>.<name>`.
  Collision-proof without a registry.

## Lifecycle: one boot path for every document

The handshake is **page-initiated**, and it is the only boot path. First open,
raw F5, dev hot-reload, crash recovery, and repeated navigation are the same
sequence:

```
document loads
  └─ helper sends  { kind: "send", name: "osfui.hello" }
       └─ host replies   ready  →  full state replay (platform keys + owning mod's keys)
            └─ events begin flowing; view renders from state; done
```

Because the document initiates, the host never has to guess whether a greeting
was consumed. That removes the machinery 1.x needs to make host-initiated
greetings safe across reloads: the four separate `SendRuntimeReady` call sites
in `src/runtime/Runtime.cpp` and the `domSeen` reset-and-flush ordering in
`tools/webview2_host/HostApp.cpp`. The host's whole obligation becomes:
answer hellos, in order, with `ready` then state.

Ordering guarantees, in both directions:

1. `ready` precedes all state for that document.
2. All replayed state precedes the first event.
3. `markReady()` (manifest `readySignal:true`) gates reveal exactly as
   `view.ready` does today; the reveal watchdog and error-handoff behavior are
   unchanged.
4. `ready` rejects with `no-bridge` in a plain browser instead of hanging.

## Backend symmetry

Every backend expresses the same four kinds:

| | Command handler | Request handler | Emit event | Set state |
|---|---|---|---|---|
| **Platform** | internal registry | internal registry | internal | internal |
| **Native plugin (C ABI)** | `RegisterCommand` | `RegisterRequest` | `SendToWeb` | `SetViewState` **(new)** |
| **Papyrus** | `ListenForViewActions` → `OnOSFUIViewAction` | `ListenForViewRequests` → `OnOSFUIViewRequest` | `SendViewEvent` **(new)** | `SetView*` |

The two new entries close real gaps:

- **`SendViewEvent(mod, name, args)` (Papyrus).** With the legacy transient
  `PushToView`/`PushFormsToView` removed, Papyrus would otherwise have no
  event channel and authors would encode one-shot happenings as state —
  which replays on F5, re-firing the "event". Events arrive at
  `on('<mod>.<name>')` and are never replayed or cached.
- **`SetViewState(mod, key, payloadJson)` (C ABI).** 1.x state is
  Papyrus-only; a native plugin must hand-roll reload handling (listen for a
  view-defined hello, re-push). With native state, the plugin sets a value
  once and the runtime replays it to every fresh document. This is the
  systemic fix for the blank-after-F5 bug class: backend-owned data that
  changes over time is state, not something the view must remember to
  re-request. Requests stay reserved for on-demand reads and mutations.

Retained-state mechanics carry over from 1.5 (`src/api/PapyrusApi.cpp`):
per-`mod\nkey` latest-wins cache, case-insensitive keys, bounded entry count,
main-thread form serialization with `null` slot-keeping. The `SetViewForms`
path means form identity payloads survive the removal of `PushFormsToView`.

Papyrus keeps its 1.5 names. The modern pair
`ListenForViewActions`/`OnOSFUIViewAction` shipped recently and renaming it
(e.g. to "ViewCommands") would churn exactly the mods that already migrated,
in the one language where migration means recompiling `.pex` files. Only the
two legacy `RegisterForViewActions*` registrations are removed.

## Platform services, reclassified

The four-verb model dissolves 1.x's subscribe-on-read wart. Today
`settings.get`, `views.get`, `i18n.get`, and `diagnostics.get` are requests
with an invisible side effect: they subscribe the caller to future pushes.
In 2.0 these registries are **platform state keys** — reads-with-replay *are*
state:

```js
osfui.state.on('osfui/views', render);        // replay now + every change; no read roundtrip
osfui.state.on('osfui/settings', render);
osfui.state.on('osfui/diagnostics', render);
osfui.state.on('osfui/i18n', applyCatalog);   // consumed by the i18n namespace internally
```

The `request` verb keeps genuine one-shots and mutations, which now settle
payload-or-error:

- `settings.set` / `settings.reset` — a failed set **rejects** with the code
  (1.x resolved a `settings.ack { ok:false }` payload the caller had to
  inspect).
- `game.get`, `ping`, `osfui.setViewAutoStart`, `menu.open`/`menu.close` and
  the other view operations with failure outcomes, diagnostics reporting.

Events keep the true one-shots: `ui.hotkey`, `ui.visibility`, `ui.gamepad`,
`settings.changed` deltas, `settings.persisted`.

**User-paced flows settle fast.** `settings.captureKey` waits on a human
pressing a key — an indefinitely pending request is the wrong shape (and
fights the client timeout). The 2.0 pattern: the request settles in machine
time ("capture armed", or `capture-busy`), and the outcome arrives as the
`settings.captured` event. Requests settle in machine time; human-time
outcomes are events.

Commands (pure notifications) remain: `close`, `log`, `view.ready`,
input-mode declarations (`osfui.gamepadRaw`, `osfui.handleBack`), and the
fixed-target shell verbs.

## Failure semantics

- `send` returns `false` only when the message cannot be posted locally.
  There is no remote signal, by design — wanting one means it's a request.
- `request` resolves with the payload or rejects with a typed error carrying
  a stable `code`. Layered codes stay distinguishable:
  - `no-bridge` — local, immediate.
  - `timeout` — the client timer (default 10 s; `timeoutMs: 0` disables only
    the client timer).
  - `no-response` — the backend missed the host-side deadline (30 s).
  - `wrong-endpoint-kind`, `unknown-endpoint`, `invalid-request`,
    `request-capacity` — protocol enforcement.
  - anything else — the handler's own rejection code.
- A `send` naming a request endpoint is dropped — and surfaced (next
  section). Dropping is correct (executing a mutation whose kind the caller
  got wrong invites worse bugs); dropping *silently* is not.
- Late or duplicate replies after settlement are ignored (the 1.7 host
  already does this correctly in `src/api/BridgeApi.cpp`).
- Handlers respond or reject exactly once; commands can never be awaited.

## DevTools: F12 Chromium DevTools is the debug surface

OSF UI does not build its own inspector. The debugger mod authors already
know — the Chromium DevTools that F12 opens on the focused view in devMode
(`Runtime::DriveDevTools` → `OpenDevToolsWindow`) — is the first-class debug
surface. 2.0's job is to make sure everything an author needs to see actually
*reaches* that surface, because today a protocol mistake (wrong endpoint
kind, malformed payload, timeout, dropped send) is visible only in the SFSE
log the author isn't watching.

The rule: **every error an author can cause is routed to the view's own
console output**, where DevTools shows it with full object inspection.

### Error routing

Two sources, one sink (the page console):

- **Client-detected failures** — the helper emits a `console.error` with a
  stable `[osfui]` prefix: every request rejection (endpoint name, `code`,
  message, the rejecting payload as an inspectable object), `no-bridge`, and
  client timeouts. Unhandled promise rejections from `request()` therefore
  stop being opaque `Uncaught (in promise)` noise — the named error precedes
  them.
- **Host-detected failures** — mistakes the page would otherwise never hear
  about are delivered back to the *offending view* on a dev-only
  `osfui/debug.error` event, and the helper prints them the same way: a
  `send` dropped for `wrong-endpoint-kind`, a send to an unknown endpoint,
  a request reaped by view close, a plugin that missed the 30 s deadline,
  a Papyrus listener that never replied. Payloads are bounded the same way
  the bridge already bounds echoed names.

Because devMode already forwards console output and uncaught exceptions over
the pipe into the SFSE log (`tools/webview2_host/HostApp.cpp`,
`Runtime.cpp`), this one mechanism makes every failure visible in DevTools
*and* the log with no second channel to maintain.

In release builds the dev-only events are not emitted; the curated
diagnostics registry remains the user-facing health surface, and recurring
protocol misuse (e.g. a view repeatedly sending to a request endpoint) raises
a `view.*`-family diagnostic so it is visible on the Mods surface even
outside devMode.

### Opt-in traffic tracing

For "what is actually crossing the bridge" questions, the helper gets a
trace flag (`localStorage['osfui:trace'] = '1'`, read at helper load) that
logs every envelope in both directions via `console.debug` with the same
`[osfui]` prefix — kind, name, id, payload object, and for requests the
settlement latency when it resolves. DevTools' own console filtering,
object inspection, and preserve-log then do everything a bespoke traffic
inspector would, per view, with zero native code and zero cost when the flag
is off. State-cache questions ("why is my HUD blank") are answered the same
way: with tracing on, every replayed `state` envelope is visible at document
boot, so either the key arrives (view bug) or it doesn't (backend bug).

### F12 reach

Today F12 targets only the focused menu. HUDs and background views are
debugged through the dev harness (`osfui dev --game`), which can request
DevTools for any loaded view — the `openDevTools` pipe command already takes
a view id. 2.0 keeps that split: F12 = focused menu, harness = any view.

## Relationship to the 1.x surface

The migration table, endpoint reclassification list, ABI-major handling,
sequencing, and test matrix live in
[the 2.0 migration plan](mod-api-2.0-migration.md). Design-level
constraints that plan must honor:

- Old views load the *new* shared helper (it ships with OSF UI). Removed
  members fail loudly; the changed `request()` return shape does not — it is
  the one silent break and is documented accordingly.
- Unmigrated views get a *legible* failure, not a blank page: a load-time
  `compat.*` diagnostic keyed off manifest `targetVersion` (< 2.0), surfaced
  on the Mods surface.
- The native ABI remains additive at 1.8: `SetViewState` is appended at the
  vtable tail and `RegisterCommand` retains the 1.x auto-ack compatibility
  path. `OSFUI_RequestBridge` still refuses genuinely different majors safely
  (`src/api/Exports.cpp`).

## Open questions

Kept as authored, for the record. Two of the three were settled during
implementation and one was deliberately deferred — the reasoning is in
[the 2.0 migration plan](mod-api-2.0-migration.md), "Resolved design questions".

- Granularity of platform state keys: is `osfui/settings` one document, or
  per-mod (`osfui/settings/<mod>`) to keep push sizes bounded on large
  registries? (Leaning per-mod; the registry *shape* changes rarely.)
- Does `SetViewState` (C ABI) accept only JSON text, or grow typed overloads
  in `OSFUI_JSON.h` like `JsonRequest::Respond` did?
- Should the `osfui:trace` flag also be settable from the dev harness
  (per-view, without touching page storage), mirroring how the harness can
  already open DevTools for any view?
