# OSF UI — agent runbook

HTML/CSS/JS UI framework for Starfield: an SFSE/CommonLibSF plugin that renders web views
through an out-of-process Microsoft Edge WebView2 host and composites them over the game with a
D3D12 present hook. Architecture: `docs/architecture.md`. Security posture: `docs/security-model.md`.

## Changelog — keep it current

**Every notable change MUST be recorded in `CHANGELOG.md` under the `## Unreleased` heading, in
the same change that makes it.** Do not defer it to "later" or a release step — an unreleased
change that isn't in the changelog is a change that will be forgotten at release time.

- **Notable** = anything a player, view author, or plugin author would notice or need to know:
  bug fixes, new/changed/removed behavior, security changes, native/bridge API or protocol
  changes, new or changed `ui.command`s, config changes.
- **Skip**: purely internal refactors, test-only changes, comment/doc typo fixes, and changes
  with no observable effect.
- **Voice**: describe the user-visible outcome and *why*, not the code — match the existing
  entries (narrative, plain language, name the symptom that changed).
- **Sections**: reuse the headings already in use — `Fixed`, `Security`, `Other changes`,
  `For view authors`, `For plugin authors`, `Highlights`. Add a new heading only when a change
  genuinely doesn't fit one. Security-relevant changes go under `Security`.
- If a change spans multiple commits (e.g. a fix plus its follow-ups), one consolidated entry
  describing the final behavior is better than one per commit.

## Build / deploy / test

- `xmake build` builds the plugin **and** the `osfui-webview2-host` exe and auto-deploys both to
  `MO2\mods\OSF UI` (the WebView2 host lands in `SFSE/Plugins/OSFUI/bin/`). Building the host
  target alone: `xmake build osfui-webview2-host`.
- Native host-side unit tests live in `tests/native/` (`run.sh`, needs a C++23 compiler — no
  clang on this box; use the MSVC dev shell, see memory `mcm-native-m1-progress`). They compile
  real runtime sources against stubs; no game required.
- Built-in views are generated from the Vite + TS + Preact workspace in `frontend/`; the
  committed `data/OSFUI/views/` is build output (`npm run check:dist` byte-compares in CI).
- The out-of-process host can be driven headlessly without launching the game via the pipe driver
  at `C:\Users\Public\osfui-host-driver` (see `driver.ps1`) — useful for verifying host behavior
  (focus, egress enforcement, crash recovery) end-to-end. In-game verification still needs MO2 + SFSE.
