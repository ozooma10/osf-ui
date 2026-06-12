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
2. **No network by default.** `config.allowNetwork` and the per-view
   `permissions.network` flag are recognized but force-disabled with a warning
   — nothing in the codebase performs network I/O at all. When a real renderer
   lands, its network access stays compiled off/denied. [enforced by absence +
   forced-off flags]
3. **No filesystem access for views** except their own folder's local assets
   (`index.html`, css, js, images). Manifest `entry` may not be absolute or
   contain `..`. The Phase-1 Ultralight `FileSystem` must canonicalize and
   prefix-check every path against the view root. [entry validation enforced;
   FS sandbox is a Phase-1 TODO]
4. **No process execution.** No bridge command may spawn processes; none is
   planned. [enforced by absence]
5. **No arbitrary native bridge.** There is exactly one inbound message type
   (`ui.command`) with an explicit whitelist: `close`, `log`, `ping`,
   `setVisible`. Unknown types and commands are rejected and logged. There is
   intentionally no "call function by name", no eval, no reflection.
   [enforced in `MessageBridge::HandleUiCommand`]
6. **Per-view permissions** (`nativeBridge`, `filesystem`, `network`) default
   to deny in the manifest parser. Today `nativeBridge=false` prevents bridge
   creation; finer-grained, per-command grants come later with multi-view
   support. [partially enforced]

## Future hardening (when the real renderer lands)

- Canonical-path sandbox for the Ultralight FileSystem (reject symlinks/ADS
  tricks, prefix check after canonicalization).
- Null clipboard provider until an explicit opt-in design exists.
- Rate-limit bridge messages per view (JS must not be able to stall the game
  thread with message floods).
- Message size caps (already: log text truncated at 512 chars; generalize).
- Versioned bridge API so views can't probe for undocumented commands.
