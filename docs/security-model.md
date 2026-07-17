# Security Model

## Threat model

Views (HTML/CSS/JS) are **mod-provided content downloaded from the internet**.
Treat every view as untrusted code running next to a game process with a
native plugin attached. The job of this runtime is to make the worst case "an
ugly overlay", never "arbitrary native code execution".

## Rules (current enforcement status in brackets)

1. **JS is untrusted.** Nothing a view sends is executed, evaluated, or used
   as a format string natively. Bridge input is parsed defensively
   (non-throwing JSON parse, typed accessors with defaults, length-bounded
   logging). [enforced in `MessageBridge` / `Json`]
2. **No network by default.** The per-view `permissions.network` flag is
   recognized but force-disabled with a warning; there is no config switch.
   ⚠ Known gap since Phase 1: WebCore has its own HTTP stack and Ultralight's
   request-blocking `NetworkListener` is Pro-edition-only, so http(s) is not
   hard-blocked at the engine level. Mitigations in place: `cacert.pem` is
   deliberately NOT shipped (no TLS roots → all https fails), views are loaded
   from `file:///` only, and the only shipped view is local content. A real
   block (content-security-policy injection or a Pro license) is future work.
   [partially enforced; documented gap]
3. **No filesystem access for views** except local view assets (`index.html`,
   css, js, images) plus the read-only Ultralight support resources (ICU data).
   [enforced in `SandboxFileSystem` (UltralightWebRenderer.cpp): only relative
   paths, no root name/directory, any `..` component rejected, two whitelisted
   base dirs (the shared `views/` dir and the ICU resources dir); manifest
   `entry` validation unchanged. **Multi-view note:** views load via
   folder-qualified URLs (`file:///<folder>/...`) against ONE shared `views/`
   base, so a view can read a *sibling* view's local assets — acceptable for
   local mod content, but it is not strict per-view isolation (which would need
   a per-request view context Ultralight's FileSystem API does not expose).
   Lexical checks only — symlink/ADS canonicalization is still future hardening]
4. **No process execution.** No bridge command may spawn processes; none is
   planned. [enforced by absence]
5. **No arbitrary native bridge.** There is exactly one inbound message type
   (`ui.command`) with an explicit whitelist: surface control (`close`,
   `setVisible`, `menu.open` / `menu.close`, `hud.show` / `hud.hide`,
   `setViewHidden`), diagnostics (`log`, `ping`), read-only game data
   (`game.get`), and the settings trio (`settings.get` / `settings.set` /
   `settings.reset`). Unknown types and commands are rejected and logged
   (warn-once per command name; the `ui.error` reply is sent every time).
   There is intentionally no "call function by name", no eval, no reflection.
   [enforced in `MessageBridge::HandleUiCommand`]
   - **Native plugin API caveat:** a separate *trusted native* SFSE plugin can
     register additional `ui.command` handlers via the exported
     `OSFUI_RequestBridge` API (`docs/native-plugin-api.md`); reserved
     prefixes (`ui.` / `runtime.` / `game.` / `settings.`) are refused
     [enforced in `BridgeApi::RegisterCommand`]. This widens the surface only
     by what a mod author's own DLL deliberately exposes — untrusted JS still
     cannot register anything, and each added command is that plugin's
     responsibility to validate.
   - `settings.set` is the only command that WRITES: it can only set a key
     that EXISTS in the mod's schema, to a value the `SettingsStore` validates
     and clamps to that key's declared type/range (enum ∈ options, numbers ∈
     [min,max], strings length-bounded). Untrusted JS cannot write arbitrary
     keys, out-of-range values, or to any path but the one settings file.
     [enforced in `SettingsStore::Validate`/`Set`]
6. **Per-view permissions** (`nativeBridge`, `filesystem`, `network`) default
   to deny in the manifest parser. Today `nativeBridge=false` prevents bridge
   creation; finer-grained, per-command grants come later with multi-view
   support. [partially enforced]

## Future hardening (when the real renderer lands)

- Canonical-path sandbox for the Ultralight FileSystem (reject symlinks/ADS
  tricks, prefix check after canonicalization — current checks are lexical).
- Null clipboard provider until an explicit opt-in design exists (Phase 1
  sets NO clipboard handler, which Ultralight treats as no clipboard at all).
- Rate-limit bridge messages per view (JS must not be able to stall the game
  thread with message floods). Phase 1 already caps both bridge queues at 64
  messages (drop + warn-once beyond that); per-time-window limits remain TODO.
- Message size caps (already: log text truncated at 512 chars; generalize).
- Versioned bridge API so views can't probe for undocumented commands.
