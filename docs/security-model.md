# Security Model

## Threat model

Views (HTML/CSS/JS) are mod content downloaded from the internet. Every view is treated as untrusted code running next to the game process with a native plugin attached. The runtime's goal is that a hostile view can at worst draw an ugly overlay, never execute native code.

## Rules

Each rule notes where it is enforced and any known gaps.

1. **JS is untrusted.** Nothing a view sends is executed, evaluated, or used as a format string natively. Bridge input is parsed defensively: non-throwing JSON parse, typed accessors with defaults, length-bounded logging. Enforced in `MessageBridge` / `Json`.

2. **No network by default.** The per-view `permissions.network` flag is recognized but force-disabled with a warning, and there is no config switch to turn it on. Known gap since Phase 1: WebCore has its own HTTP stack and Ultralight's request-blocking `NetworkListener` is only available in the Pro edition, so http(s) is not hard-blocked at the engine level. Mitigations: `cacert.pem` is deliberately not shipped, so all https fails for lack of TLS roots; views are loaded from `file:///` only; and the only shipped view is local content. A real block (content-security-policy injection or a Pro license) is future work.

3. **No filesystem access for views** beyond local view assets (`index.html`, css, js, images) and the read-only Ultralight support resources (ICU data). Enforced in `SandboxFileSystem` (UltralightWebRenderer.cpp): relative paths only, no root name or root directory, any `..` component rejected, and two whitelisted base directories (the shared `views/` dir and the ICU resources dir). Manifest `entry` validation is separate and unchanged.

Multi-view note: views load via folder-qualified URLs (`file:///<folder>/...`) against one shared `views/` base, so a view can read a sibling view's local assets. That is acceptable for local mod content, but it is not strict per-view isolation; strict isolation would need a per-request view context that Ultralight's FileSystem API does not expose. The checks are lexical only; symlink and ADS canonicalization is future hardening.

4. **No process execution.** No bridge command spawns processes, and none is planned.

5. **No arbitrary native bridge.** There is exactly one inbound message type (`ui.command`) with an explicit whitelist: surface control (`close`, `setVisible`, `menu.open` / `menu.close`, `hud.show` / `hud.hide`, `setViewHidden`), diagnostics (`log`, `ping`), read-only game data (`game.get`), and the settings trio (`settings.get` / `settings.set` / `settings.reset`). Unknown types and commands are rejected and logged (warn-once per command name; the `ui.error` reply is sent every time). There is no "call function by name", no eval, no reflection. Enforced in `MessageBridge::HandleUiCommand`.

   Two qualifications:

   - A separate trusted native SFSE plugin can register additional  `ui.command` handlers via the exported `OSFUI_RequestBridge` API  (`docs/native-plugin-api.md`). Reserved prefixes (`ui.`, `runtime.`,  `game.`, `settings.`) are refused in `BridgeApi::RegisterCommand`. This  widens the surface only by what a mod author's own DLL deliberately  exposes; untrusted JS still cannot register anything, and validating  each added command is that plugin's responsibility.

   - `settings.set` is the only command that writes. It can only set a key that exists in the mod's schema, to a value the `SettingsStore` validates and clamps to that key's declared type and range: enums must be one of the declared options, numbers are clamped to [min, max], and strings are length-bounded. Untrusted JS cannot write arbitrary keys, out-of-range values, or to any path other than the one settings file. Enforced in `SettingsStore::Validate` / `Set`.

6. **Per-view permissions** (`nativeBridge`, `filesystem`, `network`) default to deny in the manifest parser. Today `nativeBridge=false` prevents bridge creation; finer-grained, per-command grants come later with multi-view support. Partially enforced.

## Future hardening

- Canonical-path sandbox for the Ultralight FileSystem: reject symlink and ADS tricks by prefix-checking after canonicalization. Current checks are lexical.
- Null clipboard provider until an explicit opt-in design exists. Phase 1 sets no clipboard handler, which Ultralight treats as no clipboard at all.
- Rate-limit bridge messages per view, so JS cannot stall the game thread with message floods. Both bridge queues are already capped at 64 messages (drop and warn once beyond that); per-time-window limits remain to do.
- Message size caps. Log text is already truncated at 512 chars; generalize this.
- Versioned bridge API so views cannot probe for undocumented commands.
