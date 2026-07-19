# Security Model

## Threat model

Views (HTML/CSS/JS) are mod content downloaded from the internet. Every view is treated as untrusted code running next to the game process with a native plugin attached. The runtime's goal is that a hostile view can at worst draw an ugly overlay, never execute native code.

## Two backends, two postures

Most of this document was written for the Ultralight backend, whose sandbox is
built out of an embedder-supplied `FileSystem` and an engine with no TLS roots.
The shipped default is now `renderer: "webview2"`, which is **full Chromium**.
Several Ultralight-specific mitigations simply do not exist there, and pretending
otherwise would be the worst kind of security documentation. Rules 2, 3 and 7
below are therefore split per backend; the rest hold on both, because they are
enforced in native C++ above the renderer.

| | `webview2` (default; out-of-process host) | `ultralight` |
|---|---|---|
| View origin | `https://osfui.local/…`, mapped to the views root | opaque `file:///…` |
| Filesystem reach | whole views root, via the virtual-host mapping | whole views root, via `SandboxFileSystem` lexical checks |
| TLS roots | the OS trust store — **https works** | none shipped — https fails for lack of roots |
| Request interception | none (no `WebResourceRequested` filter, no CSP injection) | none (`NetworkListener` is Ultralight Pro only) |
| Process boundary | separate `osfui_webview2_host.exe` | in-process with the game |

The honest summary: on WebView2 there is **no engine-level network block at
all**. What is actually enforced on both backends is the native bridge surface
(rules 1, 4, 5, 6) — which is where the "hostile view executes native code"
threat lives — plus the fact that view content is local mod content the user
installed.

One structural mitigation is new and applies to the built-in views only:
`data/OSFUI/views/` is **generated build output that is committed to git**, and
CI rebuilds it and byte-compares on every push. Shipped view code therefore
shows up in review diffs and cannot be quietly altered in the tree without
either failing the staleness gate or appearing as a reviewable change to
`frontend/src/`. It says nothing about third-party views.

## Rules

Each rule notes where it is enforced and any known gaps.

1. **JS is untrusted.** Nothing a view sends is executed, evaluated, or used as a format string natively. Bridge input is parsed defensively: non-throwing JSON parse, typed accessors with defaults, length-bounded logging. Enforced in `MessageBridge` / `Json`.

2. **No network by declaration; not hard-blocked on either backend.** The per-view `permissions.network` flag is recognized but force-disabled with a warning, and there is no config switch to turn it on (`ViewManifest.cpp`). That flag is *declaratory* — nothing downstream consults it to filter requests. Per backend:

   - **`ultralight`** — WebCore has its own HTTP stack and the request-blocking `NetworkListener` is Pro-edition only, so http(s) is not blocked at the engine level. Mitigations: `cacert.pem` is deliberately not shipped, so all https fails for lack of TLS roots, and views load from `file:///` only.
   - **`webview2`** — **that mitigation does not hold.** This is a full Chromium using the OS certificate store, so `https://` and `fetch()` to a remote host work normally from a view. There is no `WebResourceRequested` filter installed and no CSP injected on either the in-process renderer or the out-of-process host, so nothing intercepts requests. The page is served from a real https origin, which also means the ordinary web platform is available to it (service workers, storage, WebSockets).

   What remains true on both: shipped views are local content and make no requests, and the built-in views' build gates fail on `@font-face` or a remote `url()`. A real block on WebView2 (a `WebResourceRequested` filter that denies anything not under `osfui.local`, and/or an injected CSP) is the obvious hardening and is not done.

3. **No filesystem access for views** beyond the views root. Per backend:

   - **`ultralight`** — enforced in `SandboxFileSystem` (`UltralightWebRenderer.cpp`): relative paths only, no root name or root directory, any `..` component rejected, and two whitelisted base directories (the shared `views/` dir and the read-only ICU resources dir).
   - **`webview2`** — enforced by `SetVirtualHostNameToFolderMapping`, which exposes exactly one folder (the views root) under `osfui.local` and nothing else on disk. There is no lexical path filter; the mapping *is* the boundary. Requests that escape it are simply requests to some other origin, which is the network question (rule 2), not a filesystem one.

   Manifest `entry` validation is separate, native, and unchanged — it applies on both backends.

Multi-view note: on both backends every view is served out of one shared `views/` root under folder-qualified URLs, so a view can read a sibling view's local assets. That is acceptable for local mod content, but it is not strict per-view isolation; strict isolation would need a per-request view context that neither Ultralight's FileSystem API nor a single virtual-host mapping provides. The Ultralight checks are lexical only; symlink and ADS canonicalization is future hardening.

4. **No process execution.** No bridge command spawns processes, and none is planned.

5. **No arbitrary native bridge.** There is exactly one inbound message type (`ui.command`) with an explicit whitelist: surface control (`close`, `setVisible`, `menu.open` / `menu.close`, `hud.show` / `hud.hide`, `setViewHidden`), read/subscribe catalogs (`views.get`, `i18n.get`), diagnostics (`log`, `ping`), read-only game data (`game.get`), per-view input routing (`osfui.gamepadRaw`, `osfui.handleBack`), and the settings commands (`settings.get` / `settings.set` / `settings.reset` / `settings.captureKey`). Unknown types and commands are rejected and logged (warn-once per command name; the `ui.error` reply is sent every time). There is no "call function by name", no eval, no reflection. Enforced in `MessageBridge::HandleUiCommand`.

   Three qualifications:

   - A separate trusted native SFSE plugin can register additional `ui.command` handlers via the exported `OSFUI_RequestBridge` API (`docs/native-plugin-api.md`). Plugin commands must be shaped `<author>.<modname>.<name>` — two dots minimum, with the leading mod id matching the public id grammar. Every platform command is dotless or single-dot, so platform commands are structurally unregisterable (this structural rule replaced an earlier reserved-prefix list, which could drift). Duplicate registrations are refused first-wins, so an already-claimed command cannot be hijacked. Enforced in `BridgeApi::RegisterCommand` (`IsValidPluginCommand`). This widens the surface only by what a mod author's own DLL deliberately exposes; untrusted JS still cannot register anything, and validating each added command is that plugin's responsibility.

   - Only the settings commands write, and only schema-bounded values. `settings.set` can only set a key that exists in the mod's schema, to a value the `SettingsStore` validates and clamps to that key's declared type and range: enums must be one of the declared options, numbers are clamped to [min, max], strings and key names are length-bounded, and `flags` arrays are filtered to declared options. `settings.reset` restores declared defaults. `settings.captureKey` arms a one-shot key capture, and only for a setting the schema declares as `type:"key"`; the captured name is validated like any other set. Untrusted JS cannot write arbitrary keys, out-of-range values, or to any path other than the mod's own settings file. Enforced in `SettingsStore::Validate` / `SetValueWithResult`.

   - Two commands let a view take input the framework would otherwise handle: `osfui.gamepadRaw` suppresses the default gamepad nav mapping, and `osfui.handleBack` redirects Esc / gamepad B into the page as a synthetic Escape instead of closing the top menu. Both are sticky per view and clear on page (re)load or view destroy, and neither can be asserted on another view's behalf. The bound on abuse is deliberate and native: the overlay toggle key always closes the overlay in the input layer, so a view that grabs back and then stops responding cannot strand the player in a menu.

6. **Per-view permissions** (`nativeBridge`, `filesystem`, `network`) default to deny in the manifest parser. Today `nativeBridge=false` prevents bridge creation, blocks the `window.osfui` injection for that view, and drops any outbound send targeting it; finer-grained, per-command grants come later with multi-view support. Partially enforced.

7. **Clipboard is user-gesture only.** A view the user pastes into receives whatever text is on the system clipboard (which may be sensitive), and a view the user copies from can write to it. There is no bridge command that touches the clipboard on either backend, so views cannot read or write it *via the bridge*. Per backend:

   - **`ultralight`** — a real system clipboard provider (`WinClipboard`, installed from `UltralightWebRenderer.cpp`) serves in-page copy/cut/paste; this supersedes the earlier "no clipboard handler" posture. Ultralight invokes it from its editing path when a focused editable field receives Ctrl+C/X/V. First read and first write are logged. Whether WebCore blocks a programmatic `document.execCommand("paste")` without a user gesture has not been verified and is a known gap.
   - **`webview2`** — clipboard access is Chromium's own, not ours: no OSF UI code participates, so nothing is logged and the gesture rules are whatever that Chromium build enforces (the async Clipboard API normally requires a user gesture and, for reads, a permission prompt — unverified here). Note the view runs on a real https origin, so it is not automatically in the restricted bucket an opaque origin would be.

   Per-view clipboard gating is future hardening on both.

## Future hardening

- **A real network block on WebView2** (rule 2): a `WebResourceRequested` filter that denies any request not under `osfui.local`, and/or an injected CSP. This is the largest open gap, because the Ultralight-era "no `cacert.pem`" mitigation does not carry over to Chromium and nothing replaced it.
- Canonical-path sandbox for the Ultralight FileSystem: reject symlink and ADS tricks by prefix-checking after canonicalization. Current checks are lexical.
- Per-view clipboard gating (rule 7): verify WebCore denies programmatic paste without a user gesture, and consider a manifest permission so passive HUD views get no clipboard at all.
- Rate-limit bridge messages per view, so JS cannot stall the game thread with message floods. Both bridge queues are already capped at 64 messages (drop and warn once beyond that); per-time-window limits remain to do.
- Message size caps. Log text is already truncated at 512 chars; generalize this.
- Versioned bridge API so views cannot probe for undocumented commands. Partially done: `runtime.ready` already carries `bridgeVersion` (and the plugin `version`); the remaining gap is that unknown-command `ui.error` replies still let a view enumerate what exists.
