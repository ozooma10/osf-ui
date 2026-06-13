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
   `permissions.network` flag are recognized but force-disabled with a warning.
   ⚠ Known gap since Phase 1: WebCore has its own HTTP stack and Ultralight's
   request-blocking `NetworkListener` is Pro-edition-only, so http(s) is not
   hard-blocked at the engine level. Mitigations in place: `cacert.pem` is
   deliberately NOT shipped (no TLS roots → all https fails), views are loaded
   from `file:///` only, and the only shipped view is local content. A real
   block (content-security-policy injection or a Pro license) is future work.
   [partially enforced; documented gap]
3. **No filesystem access for views** except their own folder's local assets
   (`index.html`, css, js, images) plus the read-only Ultralight support
   resources (ICU data). [enforced in `SandboxFileSystem`
   (UltralightWebRenderer.cpp): only relative paths, no root name/directory,
   any `..` component rejected, two whitelisted base dirs; manifest `entry`
   validation unchanged. Lexical checks only — symlink/ADS canonicalization is
   still listed under future hardening]
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
  tricks, prefix check after canonicalization — current checks are lexical).
- Null clipboard provider until an explicit opt-in design exists (Phase 1
  sets NO clipboard handler, which Ultralight treats as no clipboard at all).
- Rate-limit bridge messages per view (JS must not be able to stall the game
  thread with message floods). Phase 1 already caps both bridge queues at 64
  messages (drop + warn-once beyond that); per-time-window limits remain TODO.
- Message size caps (already: log text truncated at 512 chars; generalize).
- Versioned bridge API so views can't probe for undocumented commands.
