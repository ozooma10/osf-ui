# Changelog

## Unreleased

### Fixed

- A quick left-stick navigation flick now moves focus exactly once instead of skipping over controls; deliberate holds still repeat after a longer pause, while release jitter and diagonal input no longer create extra steps.

## 1.5.0 — 2026-07-29

### Highlights

- `devMode` now reloads saved view files in game and supports **F12** Edge DevTools. New views and `manifest.json` changes still require a restart.
- `npm create osfui@latest` now scaffolds TypeScript menu/HUD projects with Papyrus or native backends, a Vite harness, validation, MO2 sync, and packaging.

### Added

- Settings schemas can define tabbed `pages`; unassigned groups appear under **General**.
- Added a consent-based **Report a bug** flow with local redaction, bounded log attachments, abuse limits, and private 30-day storage.
- Native plugins can publish self-clearing issues to the shared **System Health** pane.

### Fixed

- Native WebView2 form popups - including settings dropdowns, datalists, and date, time, or colour pickers authored by third-party views — now temporarily receive physical pointer ownership while open. Their options can therefore be clicked in game instead of requiring keyboard selection, without view authors replacing standard HTML controls.
- MO2 hot reload now updates each deployed mod view tree in place and copies every replacement into the browser mirror before pruning old bundles, preventing USVFS path failures from disconnecting the current page and avoiding `ERR_FILE_NOT_FOUND` for renamed shared bundles.
- `osfui dev --game` now syncs view assets while Starfield holds native files open, without replacing a live directory that the game has already enumerated.
- Mouse-wheel input now uses the live cursor position and a consistent scroll distance.
- Removed swap-chain Present hooks that conflicted with OptiScaler, Steam Overlay, ReShade, RTSS, and similar tools.
- WebView2 composition-controller failures now close the loading overlay, restore input, and show repair guidance.
- Action buttons in a mod's settings no longer report a false **No response from &lt;mod&gt;** after working correctly. A plugin that acts on a button without sending its own reply — the documented minimum — was left waiting for the full timeout.
- Closed a rare instability when a mod registered a view after the overlay had already drawn a frame; the capture path could read the view list while it was being rebuilt.
- Keybind rows in non-English languages now show a translated **Gameplay** badge instead of leaving one of the two badges in English.
- Retained data pushed to a view no longer keeps a second copy of the whole payload in memory, which was noticeable for large lists such as a full inventory.
- Closing the Mods surface with the mouse or controller while a key rebind was waiting for input now cancels the rebind. Previously the next gameplay keypress was swallowed and silently committed as the new binding.
- Reopening the Mods surface after closing it with the undo panel open no longer leaves LB/RB category switching unresponsive.
- Overlay gamepad navigation now follows the controller the player is actually using. With a second device connected — a charging pad, a wheel, or a virtual device — it previously read only the lowest slot and went dead.
- A view opened on demand can now hide and show itself with `setViewHidden`; the command previously refused any view not preloaded at startup.

### Security

- Diagnostic uploads now require a default-deny native confirmation even if the packaged settings view is replaced, redact both slash forms of private roots, and keep accepted reports private until an administrator reviews them for publication.
- A view declaring `bridge: false` is now genuinely denied the `window.osfui` bridge; the injection previously happened anyway and only the game side dropped its messages.
- Blocked outbound requests are logged at most 32 origins per view and then go silent, so a page cannot grow the logs or hitch the frame rate by requesting an endless series of hostnames.
- Links now open in the system browser only from a real click. A scripted `window.open` — which never issues a request the network filter could see — is dropped, closing a data-exfiltration channel around the default-deny egress policy.

### For plugin authors

- Added optional `OSFUI_JSON.h` helpers for typed JSON commands, requests, replies, state, schemas, and diagnostics.
- Native ABI **1.7** adds asynchronous request/reply support with 30-second timeouts and 64 in-flight requests per view, plus namespaced `ReportIssue`, `ClearIssue`, and `ClearIssuesExcept` diagnostics APIs.

### Other changes

- Logs now keep the previous session, use full dates, tag third-party content errors, and reduce routine trace noise.
- Diagnostics now lists and can trigger every discovered third-party view.

### For view authors

- Generated Papyrus projects now include concise setup documentation, Spriggit source, ESM/PEX builds, and prerequisite checks.
- Generated assets now stay under `views/<modId>/assets/`; dependent mods no longer copy OSF UI's shared kit.
- Generated HUD projects now provide passive telemetry, persistence, settings, hotkeys, and browser mocks.
- CLI output paths are checked for unsafe overlap, development sync removes stale files, and malformed options fail early.
- `dev:game --deploy` now accepts an MO2 `mods` directory and creates the project mod folder.
- Papyrus and native presets now include matching, buildable backends and package all mod files from `mod/`.
- The project creator now uses guided prompts, safe defaults, local CLI linking, and matching JavaScript/TypeScript starters.
- Bridge protocol **1.5** adds qualified native request/reply behavior, `emit`, `call`, typed events, cached Papyrus state, actions and request/reply helpers, plus built-in reporter messages kept private to `osfui/settings`.
- The author dev server no longer serves files from outside the project — private keys, `.env` files and certificates next to a project were reachable — and no longer resolves an absolute path out of a mod's asset route.
- `osfui build` refuses to delete a non-empty output directory it cannot prove it wrote, so an `outDir` pointing outside the project can no longer wipe a sibling's output.
- `osfui dev:game` now merges its answers into `.osfui/local.json` instead of overwriting it, and reports a malformed file rather than silently replacing it.
- `npm create osfui` rejects unknown or misspelled flags instead of scaffolding the wrong surface, and enforces the 64-character mod id limit the runtime applies.
- A mod id beginning with a digit now scaffolds a Papyrus script name the Creation Kit compiler accepts; such projects previously could not build at all.
- The pre-build content check now targets real outbound requests, so an inline SVG namespace or a link in view text no longer fails the build.
- A `manifest.json` written as TypeScript or JavaScript config no longer drops `accent`, and `icon` no longer produces a spurious unknown-key warning in `devMode`.
- A settings schema declaring a page id starting with an underscore no longer collides with the implicit **General** tab; page ids must now begin with a letter or digit.
- With `devMode` on, a newly registered view is picked up for file reloads immediately instead of after the next scan interval.
- Scaffolded HUD projects now toggle visibility through `hud.show`/`hud.hide` instead of `setViewHidden`, whose effect the menu policy undid whenever the player closed the Mods surface; the authoring docs now explain the difference.

## 1.4.0 — 2026-07-24

Added System Health and restored stable main-thread and compositor behavior.

### Added

- Added **System Health** with actionable issues, per-mod markers, resolved history, safe recovery actions, and copyable diagnostics.

### Fixed

- Restored the stable Scaleform compositor and prevented one key press from immediately reopening or closing the menu.
- Moved per-frame runtime work and native plugin callbacks back to Starfield's main thread.

### For view authors

- Bridge protocol **1.4** adds `diagnostics.get`, `diagnostics.data`, and `osfui.openLogFolder`.

## 1.3.0 — 2026-07-22

Added richer Papyrus data, reliable interactive focus, and developer diagnostics.

### Added

- Bridge protocol **1.3** adds `PushFormsToView`, `GetFormById`, and `GetFormsById` for session-scoped game forms.
- View actions can send argument arrays through `RegisterForViewActionsArgs`.

### Fixed

- Fixed mouse-wheel input after focus moves to WebView2.
- Fixed a stack-overflow crash with BetterConsole.
- The WebView2 helper no longer backgrounds Starfield during launch.
- Interactive views now receive native keyboard/IME focus while gamepads continue through XInput.
- Fixed crosshair cursors appearing as a loading spinner.
- Prewarmed the first-load transition.
- Fixed pause-menu close/reopen races and stale callbacks.
- Fixed a startup race in Scaleform hooks and removed `uiPassProbe`.
- Failed settings writes now preserve the previous file.
- Invalid host messages are dropped instead of crashing the game.
- Fixed synthetic key input and stale text-field focus.

### Other changes

- Added the **Web Performance Lab** developer view.
- Added **Debug mode** for showing developer-only views.
- Added persistent render statistics for capture, transport, compositor, and frame delivery.

### For view authors

- Manifests can set `"debugOnly": true`.

## 1.2.1 — 2026-07-22

Improved first-time loading and Frame Generation support.

### Fixed

- WebView2 now uses Starfield's GPU, fixing invisible menus on hybrid-GPU systems.
- Drop-in views can open on demand without config edits or a companion plugin.
- AMD FSR3 Frame Generation now composites the overlay through Starfield's UI layer.

### Other changes

- First-time loads now show a themed loading panel with retry and cancel actions.
- Disabled normal `uiPassProbe` capture work.

### For view authors

- Bridge protocol **1.2** adds manifest `accent`, `readySignal`, and `osfui.viewReady()`.

## 1.2.0 — 2026-07-21

Restored controller input, enforced network isolation, and improved renderer recovery.

### Fixed

- Controller navigation now resumes after text entry.
- Menu gamepad input no longer reaches gameplay.
- Crashed or hung views now recover or close without trapping input.
- Fixed a pause-menu rebuild crash.
- Reduced crashes with Frame Generation by skipping generated swap chains.
- Restored visible focus styling without stealing game input.

### Security

- Views can no longer access the network or use WebSocket, WebRTC, WebTransport, Worker, or SharedWorker. External links still open in the system browser.

### Other changes

- Missing WebView2 Runtime now shows an installer prompt.
- `devMode` now mirrors browser console output to `OSF UI.log`.
- Expanded the shared-texture ring to tolerate brief game stalls.
- Coalesced high-frequency mouse movement to one host update per game frame.

## 1.1.1 — 2026-07-20

### Fixed

- Elevated game sessions now launch an elevated WebView2 host, fixing invisible overlays.

### Other changes

- Added a separate WebView2 host log beside `OSF UI.log`.
- Host startup errors now include useful diagnostics and clear Mark-of-the-Web from the mirrored executable.

## 1.1.0 — 2026-07-20

Moved rendering to WebView2 and added Papyrus-driven live data.

### Highlights

- Replaced Ultralight with an out-of-process Microsoft Edge WebView2 renderer.
- Added `PushToView` data and `ui.action` callbacks for Papyrus mods.

### Fixed

- Mod hotkeys no longer fire while typing in the game console.
- OEM punctuation keys are now bindable.
- Fixed rare input loss after closing the Mods menu.

### For view authors

- Added `osfui.openModPage`.

### Other changes

- **Needs update** links now open the OSF UI Nexus page.
- Removed the Ultralight backend and packaging path.
- Release builds now install and verify the WebView2 host.
- Built-in views now use Vite, TypeScript, and Preact.

## 1.0.0 — 2026-07-17

Initial release.

### Highlights

- Added HTML/CSS/JavaScript views inside Starfield.
- Added the **F10** Mods surface and visual keybinding editor.
- Added keyboard and controller navigation.

### For view authors

- Added manifest-based view packages with multi-view composition.
- Added `targetVersion` compatibility notices.
- Added JavaScript bridge protocol **1.0**.
- Added persistent, versioned settings schemas.
- Added shared CSS and TypeScript definitions.
- Added `devMode`, **F11** reload, schema reload, and a browser harness.

### For plugin authors

- Added the native C++ API for registering views and handling commands.

### Engine integration

- Views use the game menu stack, pause state, input controls, and hardware cursor.
- User settings persist separately from the shipped developer config.
- Ultralight rendering was an opt-in build.
