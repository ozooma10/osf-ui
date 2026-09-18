# OSF UI 1.6.0 hotfix candidate

Branch: `hotfix-1.6.0`. Base: released `v1.5.0`, commit
`ab9f5992929c6b43112db26b0b82e72d60cb6446`, verified against the GitHub tag.
Scope: cache cleanup and Luma HDR compatibility, including startup corrections
found by the release smoke test. Changes remain uncommitted.

## Included

Version 1.5.0 creates `%LOCALAPPDATA%\OSFUI\views-mirror-<pid>` for each
MO2 game process. It only attempts cleanup in renderer shutdown, but SFSE
does not provide a shutdown callback and the plugin documents that its
runtime shutdown is normally unreachable. Normal exits as well as crashes
can therefore leave a complete copy of all view files behind.

The hotfix scavenges abandoned production mirrors before preparing the next
MO2 session. Live or inaccessible process owners retain their mirrors;
failed deletions are retried at the next launch. The old shared `views-mirror`
directory is only eligible when no helper is detected. Settings, WebView2
browser data, helper binaries, unknown directories, research instance
mirrors, and the newer development branch's caches are outside this cleanup.

This adapts the legacy scavenging portion of `08e101d` without importing its
fingerprinted cache redesign. Release
metadata is 1.6.0; the public bridge protocol remains 1.5, and the native SDK
and Papyrus files match the 1.5.0 tag.

Cleanup happens on the next MO2 launch, so the latest session's mirror can
remain on disk after exit until the following launch.

The Luma fix is adapted from `028f132`: install the Scaleform hooks at SFSE
`kPostLoad`, after Luma has patched the vanilla composite implementation;
chain only Luma's known composite hook; and use matching RGBA16F render-target
views and pipeline states for its HDR UI buffers. Stock RGBA8 support remains.
The small `ModuleNameForAddress` helper from `5d16501` is included solely to
identify the existing hook owner; none of that commit's API rewrite is included.

Menu opens are refused when hook installation fails. Startup menu requests
are retained while installation is pending, without capturing input, and
explicitly applied by the first normal tick after installation, even when no
new menu command arrives. Engine focus-menu, control, pause, and cursor effects
wait until post-data-load UI setup publishes readiness. This avoids changing
input-enable masks while player-control listeners are still initializing.
The existing ScaleformEnd draw location is retained; later broad renderer
changes are outside this backport.

## Verification on 2026-09-18

- Production `releasedbg` build passed for the plugin and WebView2 helper,
  using the release's pinned CommonLibSF submodules. Built-in views also built.
- Focused native regression test passed with MSVC, covering abandoned and
  live owners, the legacy shared folder, unrelated data, malformed names,
  Windows file-lock failures, and successful retry after unlocking.
- The Luma policy test passed with MSVC, covering case-insensitive Luma
  composite-hook matching, rejection of unknown owners and other slots,
  stock/HDR render-target formats, and rejection of unsupported formats.
- `git diff --check` passed.
- Packaging gates passed, and all 30 archived files were hashed against
  their staged sources. The DLL's file and product versions are 1.6.0.0.
- Four fresh automated game sessions passed against the exact RC4 ZIP: stock,
  Luma with a startup menu, Luma restart, and stock restart with a startup menu.
  Settings Slim and Character Studio were disabled, including their test builds.
  The runs recorded 77 assertions, 162 completed observations, and 28 WGC captures.
- Runtime checks covered cache scavenging and locked-file retry, real prior-game
  mirror cleanup, live-owner preservation, built-in and bridge-1.5 third-party
  menus, keyboard/mouse input, settings persistence, focus loss/return, repeated
  open/close cycles, vanilla pause, and resumed gameplay movement. Luma runs
  confirmed hook chaining and a first draw into `R16G16B16A16_FLOAT`.
- RC2 exposed an early startup control-layer crash. RC3 prevented the crash but
  exposed missing startup-policy replay. Both were corrected in RC4 and the
  startup cases passed with both renderers. Earlier evidence is retained.
- Testing used separate private MO2 mods. The private profile's mod list was restored
  byte-for-byte and existing user settings retained their original hashes.
  The test game and helpers were stopped. No commit, tag, push, or publication
  occurred; the original development checkout was not edited by this work.

Candidate archive: `dist/OSF-UI-v1.6.0-rc4.zip` (supersedes RC1, RC2, and RC3).

SHA-256: `d287fa8927cf599b80b6d3ba4c81a324e58c237cd4904c24d700e73d211b4a06`

Automated desktop release smoke acceptance passed. See
[the smoke report](hotfix-1.6.0-smoke.md) for run artifacts, screenshots,
failure history, and the exact acceptance boundary. Physical HDR display
accuracy, gamepad use, other mod combinations, and long-session endurance
were not tested. These are distinct from the verified Luma render-target path.

## Other candidates for separate review

These were identified from post-release source history and are not included.
They need dependency review and fresh runtime validation before backporting.

| Candidate | Source commits | Reason to review |
| --- | --- | --- |
| Invisible-menu lockups and orphan helpers | `287d4c2`, `72e7f0b` | Prevent stale transparent frames from revealing an invisible overlay that traps input and pause; add bounded recovery and helper exit handling. Changes the private DLL/helper protocol. |
| GPU synchronization and runtime/security hardening | `b339a33` | Prevent shared textures being overwritten before GPU consumption; harden shutdown, callbacks, persistence, and WebView2 request filtering. Broad 27-file change that builds on the reveal fixes. |
| Recovery after helper loss | `738c203` | Recreate the renderer after a helper failure instead of leaving UI unavailable for the session. Adds a recovery state machine on top of the preceding fixes. |
| Pipe and host lifecycle hardening | `55cefdf` | Bound transport queues and strengthen shutdown and thread coordination. Large change with intervening dependencies; unsuitable for a blind cherry-pick into this focused candidate. |

RC4 passed the requested automated release smoke and is the candidate to use
for this hotfix. Review the first two groups separately for a later stability
update. Publishing requires an explicit user request.
