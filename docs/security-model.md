# Security Model

## Threat model

Views (HTML/CSS/JS) are mod content downloaded from the internet. Every view is treated as untrusted code running next to the game process with a native plugin attached. The runtime's goal is that a hostile view can at worst draw an ugly overlay, never execute native code.

## Renderer posture

OSF UI uses the out-of-process WebView2 host. Views run in full Chromium at
`https://osfui.local/…`, mapped to the shared views root. The host installs a
default-deny egress guard per view (rule 2 below): http(s) requests outside
`osfui.local` are answered locally with 403, and the non-HTTP transports the
request filter cannot see (WebSocket, WebRTC, WebTransport) have their
constructors removed from every document.

The controls that prevent a hostile view from executing native code are the
native bridge rules below. Chromium lives in a separate
`osfui_webview2_host.exe` process; that boundary reduces renderer failure impact
but is not a substitute for validating bridge messages.

One structural mitigation is new and applies to the built-in views only:
`data/OSFUI/views/` is **generated build output that is committed to git**, and
CI rebuilds it and byte-compares on every push. Shipped view code therefore
shows up in review diffs and cannot be quietly altered in the tree without
either failing the staleness gate or appearing as a reviewable change to
`frontend/src/`. It says nothing about third-party views.

## Rules

Each rule notes where it is enforced and any known gaps.

1. **JS is untrusted.** Nothing a view sends is executed, evaluated, or used as a format string natively. Bridge input is parsed defensively: non-throwing JSON parse, typed accessors with defaults, length-bounded logging. Enforced in `MessageBridge` / `Json`.

2. **No network, enforced default-deny.** The per-view `permissions.network` flag is recognized but force-disabled with a warning (`ViewManifest`), and the host enforces the deny with two mechanisms, because no single one covers everything (`InstallNetworkGuard` in `tools/webview2_host/HostApp.cpp`):

   - A `WebResourceRequested` filter answers every http(s) request outside the `osfui.local` virtual host locally with 403 — documents (**including top-level frame navigations**: `location = 'https://evil/?data'` is denied before any packet leaves), `fetch()`/XHR, media, SSE, images/scripts/`sendBeacon`, and (via the source-kinds registration, standard on current Evergreen runtimes) worker-initiated fetches. The check requires `/` or end-of-string immediately after the host, so `osfui.local.evil.com` and `osfui.local@evil.com` lookalikes are denied. Denials are logged warn-once per view+origin.
   - `WebResourceRequested` cannot see non-HTTP transports (WebView2 raises no event for WebSocket/WebTransport handshakes), so a document-created script removes their entry points — `WebSocket`, `RTCPeerConnection` (+`webkit` alias), `WebTransport` — as non-configurable `undefined` in every document, iframes included (`about:blank` child realms confirmed covered, so the constructor cannot be borrowed from a synthetic child frame).

   **Known gap — worker-scope transports.** The document-created script injects into documents, not Web Worker global scopes, and the request filter physically can't see WebSocket/WebTransport. So a page can spawn a dedicated/shared worker and open a `WebSocket` or `WebTransport` from inside it — that one channel survives both mechanisms (verified: `typeof WebSocket === 'function'` in a dedicated worker; worker `fetch` is still filtered). RTCPeerConnection is not exposed in workers, and service workers have no WebSocket, so those are unaffected. Blast radius is bounded: a worker can still only exfiltrate what the view itself can read (its own assets, bridge-delivered data, clipboard-on-paste) — it has no filesystem or native reach. The closer for this gap is a `connect-src 'self'` CSP response header (see Future hardening), which *does* reach worker scopes; until that lands, this is the one open egress path.

   `devMode` is deliberately not exempt (harness development happens in a desktop browser, not in-game). `target="_blank"` links still open in the OS default browser via `NewWindowRequested` — the WebView itself never fetches them. On a pre-source-kinds runtime the filter degrades to documents/fetch/XHR and logs the gap. Verified end-to-end against a live host (runtime 150.0.4078.83): remote fetch blocked before any network contact, main-frame navigation to an external URL blocked, local fetch 200, lookalike hosts denied, document + `about:blank`-iframe constructors `undefined`, worker `fetch` blocked — and the worker-transport gap above confirmed present.

3. **No local filesystem access beyond the views root.** `SetVirtualHostNameToFolderMapping` exposes exactly the shared views folder under `osfui.local`; nothing else on disk is mapped. Manifest `entry` validation separately rejects paths that escape a view folder. Because every view shares one mapped root, a view can read a sibling view's assets; strict per-view isolation would require separate mappings or request filtering.

4. **No process execution.** No bridge command spawns processes, and none is planned.

5. **No arbitrary native bridge.** There is exactly one inbound message type (`ui.command`) with an explicit whitelist: surface control (`close`, `setVisible`, `menu.open` / `menu.close`, `hud.show` / `hud.hide`, `setViewHidden`), read/subscribe catalogs (`views.get`, `i18n.get`), diagnostics (`log`, `ping`), read-only game data (`game.get`), per-view input routing (`osfui.gamepadRaw`, `osfui.handleBack`), and the settings commands (`settings.get` / `settings.set` / `settings.reset` / `settings.captureKey`). Unknown types and commands are rejected and logged (warn-once per command name; the `ui.error` reply is sent every time). There is no "call function by name", no eval, no reflection. Enforced in `MessageBridge::HandleUiCommand`.

   Three qualifications:

   - A separate trusted native SFSE plugin can register additional `ui.command` handlers via the exported `OSFUI_RequestBridge` API (`docs/native-plugin-api.md`). Plugin commands must be shaped `<author>.<modname>.<name>` — two dots minimum, with the leading mod id matching the public id grammar. Every platform command is dotless or single-dot, so platform commands are structurally unregisterable (this structural rule replaced an earlier reserved-prefix list, which could drift). Duplicate registrations are refused first-wins, so an already-claimed command cannot be hijacked. Enforced in `BridgeApi::RegisterCommand` (`IsValidPluginCommand`). This widens the surface only by what a mod author's own DLL deliberately exposes; untrusted JS still cannot register anything, and validating each added command is that plugin's responsibility.

   - Only the settings commands write, and only schema-bounded values. `settings.set` can only set a key that exists in the mod's schema, to a value the `SettingsStore` validates and clamps to that key's declared type and range: enums must be one of the declared options, numbers are clamped to [min, max], strings and key names are length-bounded, and `flags` arrays are filtered to declared options. `settings.reset` restores declared defaults. `settings.captureKey` arms a one-shot key capture, and only for a setting the schema declares as `type:"key"`; the captured name is validated like any other set. Untrusted JS cannot write arbitrary keys, out-of-range values, or to any path other than the mod's own settings file. Enforced in `SettingsStore::Validate` / `SetValueWithResult`.

   - Two commands let a view take input the framework would otherwise handle: `osfui.gamepadRaw` suppresses the default gamepad nav mapping, and `osfui.handleBack` redirects Esc / gamepad B into the page as a synthetic Escape instead of closing the top menu. Both are sticky per view and clear on page (re)load or view destroy, and neither can be asserted on another view's behalf. The bound on abuse is deliberate and native: the overlay toggle key always closes the overlay in the input layer, so a view that grabs back and then stops responding cannot strand the player in a menu.

6. **Per-view permissions** (`nativeBridge`, `filesystem`, `network`) default to deny in the manifest parser. Today `nativeBridge=false` prevents bridge creation, blocks the `window.osfui` injection for that view, and drops any outbound send targeting it; finer-grained, per-command grants come later with multi-view support. Partially enforced.

7. **Clipboard follows Chromium's gesture rules.** There is no OSF UI bridge command for clipboard access. In-page copy, cut, paste, and the async Clipboard API are handled by Chromium; OSF UI does not log or independently gate them. A user paste can disclose sensitive clipboard text to a view, and per-view clipboard gating remains future hardening.

## Future hardening

- **A `connect-src 'self'` CSP response header** injected on the served document (via the `WebResourceRequested` local-response path). This is not merely belt-and-suspenders: CSP `connect-src` reaches Web Worker scopes and covers WebSocket/WebTransport, so it is the actual closer for rule 2's known worker-transport gap — the request filter and the constructor-neuter script cannot reach that channel.
- Per-view clipboard gating (rule 7), especially for passive HUD views.
- Rate-limit bridge messages per view, so JS cannot stall the game thread with message floods. Both bridge queues are already capped at 64 messages (drop and warn once beyond that); per-time-window limits remain to do.
- Message size caps. Log text is already truncated at 512 chars; generalize this.
- Versioned bridge API so views cannot probe for undocumented commands. Partially done: `runtime.ready` already carries `bridgeVersion` (and the plugin `version`); the remaining gap is that unknown-command `ui.error` replies still let a view enumerate what exists.
