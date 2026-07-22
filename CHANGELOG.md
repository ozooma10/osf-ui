# Changelog

## Unreleased

### Fixed

- Interactive menus now give their WebView genuine foreground focus for the whole session, matching the smooth behavior observed when Starfield loses focus without overriding either process's GPU priority. Keyboard and IME input route natively, mouse movement/buttons/wheel are captured by the host, and XInput gamepads keep the existing navigation/raw-event behavior even though Starfield's focus-gated controller feed pauses. HUD-only views keep Starfield focused and cap capture at 60 Hz to bound gameplay GPU/copy pressure; interactive capture can run up to 240 Hz on supported Windows builds. Chromium also no longer treats the tiny offscreen capture host as occluded background work, while explicitly hidden views still suspend normally.
- Fixed CSS crosshair cursors appearing as the Windows loading spinner, most visibly over the Web Performance Lab's animation stage.
- The first-load transition is now painted once during startup while it is still hidden, so invoking it no longer waits for a cold WebView renderer before it can appear.
- Hardened the Pause-menu “MOD MENUS” entry against close/reopen races: periodic injection now runs only while the engine-admitted movie is advancing, and stale click callbacks are swallowed without opening the overlay or forwarding OSF UI's private action id into the game. Unexpected Scaleform faults are also left to the crash logger instead of being caught after the VM is already corrupted, which previously turned the crash into a hang.
- Closed a startup race in the Scaleform UI-seam hooks: the overlay's D3D12 command-list hooks now publish the engine's original function *before* the hook becomes reachable, eliminating a narrow window where a render thread could route through a half-installed hook, drop a GPU call or skip a UI pass, and fault the game. The render-target capture hook (a `uiPassProbe`-only diagnostic) is also no longer installed during normal play, so it no longer sits on a hot engine code path.

### Other changes

- Added a built-in **Web Performance Lab** under OSF UI's panels. It runs repeatable paint, transform, DOM layout, Canvas 2D, CSS effects and mixed-scene workloads without pausing the game; records RAF cadence, frame-time percentiles, JavaScript work, timer jitter and input-to-RAF delay; and can run or copy a complete reference suite for comparing machines and OSF UI builds.
- Mod Settings now has one persistent **Show render stats** switch under OSF UI → Diagnostics. Its primary **Fresh view** rate measures new WebView textures that actually reach the game's compositor, rather than browser animation callbacks. The panel and periodic OSF UI logs also separate capture, transport, internal overlay-pass and present cadence; report capture-to-draw and compositor CPU time; count reused frames, stalls, waits and drops; and sample the actual animated document inside same-origin iframes instead of misreporting a static launcher. It applies to every view, including views loaded after it is enabled.

## 1.2.1 — 2026-07-22

Frame Generation now carries the overlay on both real and generated frames, and first-time mod menus open cleanly without configuration edits or blank loading surfaces.

### Fixed

- Fixed the Mods menu opening as a paused, cursor-active but invisible overlay on hybrid-GPU systems. The WebView2 host now renders on Starfield's actual GPU instead of whichever adapter Windows assigns the helper process, so its shared frames can be composited; failures also report the exact HRESULT and both adapter identities.
- Drop-in views can now be opened without editing the user's `config.json` or shipping a companion SFSE plugin: `menu.open`, Papyrus `OSFUI.OpenMenu`, and the native `RequestMenu` API load a discovered `views/<modId>/<viewName>/` folder on first use. Missing ids are rejected synchronously, so Papyrus and native callers can reliably fall back instead of receiving success for an open that the runtime later ignored; Papyrus view ids are also matched correctly when `BSFixedString` interning changes their letter casing.
- Starfield's built-in AMD FSR3 Frame Generation is now fully usable with the overlay. OSF UI records into the game's transparent UI layer instead of drawing into FG-owned present chains, so opaque and translucent content stays stable across real and generated frames, through loading, rapid mouse repaint, FG toggles, and display-mode changes. The old present path also no longer retains swapchains and remains as a fail-closed fallback that suspends itself under FG if the UI seam cannot be installed.

### Other changes

- First-time menu opens now stay in-world while their WebView starts: quick loads appear directly, while slower ones use an always-warm local-link panel carrying the destination's title, accent, and input/pause behavior. Broken or never-ready views offer retry/cancel instead of exposing a blank input-capturing screen; subsequent opens remain immediate.
- `uiPassProbe` remains an off-by-default compatibility diagnostic for the Scaleform seam. Normal seam rendering no longer runs its capture windows, object scans, or characterization logging.

### For view authors

- Bridge protocol 1.2 adds optional manifest `accent` and `readySignal` fields plus `osfui.viewReady()`: views that need initial async or Papyrus data can now choose their meaningful first-paint milestone, and OSF UI holds the diegetic handoff until they report it.

## 1.2.0 — 2026-07-21

Controller play works properly again, views are cut off from the network for real, and the overlay now survives renderer crashes and game stutters. (This release also includes everything from the unpublished 1.1.2.)

### Fixed

- Controller support works again. The WebView2 renderer kept Windows keyboard focus in the browser for the whole overlay session, and Windows only delivers gamepad input to the process whose window has focus — so the game engine (and the overlay with it) went controller-deaf. The overlay now leaves focus with the game and only moves it into the browser while you actually type in a text field (click a field or start typing); controller navigation resumes the moment text entry ends.
- With a menu open, gamepad input no longer leaks into the game underneath: the thumbsticks walked the player around (and buttons could trigger game actions) behind the overlay, because the engine's control-disable flags don't gate thumbstick movement. Gamepad events are now consumed at the overlay's input receiver while a capturing menu is open, so the game never sees them; views still receive them normally (default navigation mapping and raw `ui.gamepad` alike).
- A crashed or hung view no longer strands a blank overlay that still swallows input. When a view's browser render process exits or becomes unresponsive, the host now reports it and the runtime retries the load with backoff, then cleanly removes the view if it keeps failing; a total browser-process loss hides the overlay for the rest of the session (with the cause logged) instead of leaving a dead host the game still believes is alive.
- Fixed a crash tied to the pause-menu entry while the menu list was rebuilding.
- Likely fix for crashes with Frame Generation enabled: overlay drawing is now gated to real presents and skips FG-paced swapchains. If you crashed with FG on, please try again and report.
- Keyboard and gamepad focus is visible again, and clicking inside a view no longer briefly makes the game go input-deaf. Because focus now stays with the game so controllers keep working, the browser itself was never focused — so focus outlines and `:focus`/`:focus-visible` styling didn't render (navigation was working, but looked like nothing was happening), and a click landing on a focusable element could strand Windows focus in the browser process, cutting keyboard, mouse, and gamepad until a watchdog recovered it. The overlay now emulates page focus for styling without taking OS focus, and hands focus straight back to the game if a click grabs it.

### Security

- Views can no longer reach the network. OSF UI's no-network policy was declared but not actually enforced; the WebView2 host now denies every http(s) request whose origin isn't the local `osfui.local` view root — answering with a local 403 before anything leaves the machine, page navigations included — and removes the transport and worker APIs that could otherwise slip past a request filter: `WebSocket`, `RTCPeerConnection`/WebRTC, `WebTransport`, `Worker`, and `SharedWorker`. Views must bundle their assets locally; remote fonts, images, scripts, or analytics are blocked. `target="_blank"` links still open in the system browser as before. See `docs/security-model.md` (rule 2).

### Other changes

- If the Microsoft Edge WebView2 Runtime is not installed, a dialog now appears at game launch naming the problem and offering to open Microsoft's installer download — previously the overlay just never appeared, with the cause buried in `OSF UI.log`.
- In `devMode`, a view's `console.log` / `console.warn` / `console.error` output is now mirrored into `OSF UI.log` (at INFO / WARN / ERROR), so a misbehaving view is diagnosable in game rather than only in the browser harness. Off in normal play.
- The overlay rides out brief game stutters without dropping frames: the shared-texture ring between the WebView2 host and the game grew from 3 to 4 slots, so one slow game frame no longer stalls the host's capture thread (which showed up as skipped or late overlay frames under load). Costs one extra overlay-sized texture of VRAM (~8 MB at 1080p, ~33 MB at 4K).
- Moving the mouse over the overlay is now much cheaper: a high-polling-rate mouse (500–1000 Hz) was sending one cursor-update message to the WebView2 host per raw input packet — hundreds per second of pure overhead, since the page only samples the pointer at display refresh. Cursor moves are now coalesced to a single message per game frame carrying the latest position; clicks and scrolling are unaffected and still fire immediately. In `devMode` the log periodically reports how many packets were folded into how many sends.

## 1.1.1 — 2026-07-20

### Fixed

- Running the game/MO2 as administrator no longer leaves the overlay invisible: an elevated game now launches the WebView2 host elevated via the Task Scheduler, so the host can connect (thanks to the user who reported this!).

### Other changes

- The WebView2 host now logs to `Documents\My Games\Starfield\SFSE\Logs\OSF UI.webview2-host.log`, next to `OSF UI.log` — sharing that one folder covers both.
- When the host fails to start, `OSF UI.log` now explains why: host startup errors are forwarded into it, it embeds the host log's tail, and it reports whether the host exe survived (antivirus), carries Mark-of-the-Web (SmartScreen), or the game runs as administrator. Mark-of-the-Web is stripped from the mirrored host exe automatically.

## 1.1.0 — 2026-07-20

Views now render in Chromium, and Papyrus mods can drive them with live data.

### Highlights

- **New renderer: WebView2.** Views render in Microsoft Edge WebView2 inside a separate host process, replacing Ultralight.
- **Dynamic data for Papyrus mods.** Scripts can drive a view with live data and react to its clicks.
  `OSFUI.PushToView(modId, key, values)` delivers a string list to every loaded view of the mod as a `data.push` message; 
  views fire actions back with `osfui.send('ui.action', { action, arg })`, dispatched to `OSFUI.RegisterForViewActions(receiver, fn, modId)` / `...Static` callbacks (session-scoped,  released with `Unregister`). 
  Everything is fire-and-forget: Papyrus owns the data, views re-request state by firing a `ready` action on load, and OSF UI caches nothing. 
  See the new `docs/authoring-dynamic-data.md` (worked example: porting a terminal menu).

### Fixed

- Mod hotkeys no longer fire while typing in the game console.
- The OEM punctuation keys (`- = [ ] \ ; ' , . /`) are now bindable
- Input no longer dies after closing the Mods menu in rare cases 

### For view authors

- New `osfui.openModPage` command: opens OSF UI's Nexus page in the user's system browser, for "update OSF UI" affordances in views.

### Other changes

- The "Needs update" tag in the Mods menu now links to the OSF UI Nexus page; in game it opens in the default browser.
- The Ultralight backend, its SDK build option, runtime payload, and renderer-specific packaging path are gone.
- Release builds install and verify `OSFUI/bin/osfui_webview2_host.exe`.
- Internal: built-in views are generated from a Vite + TypeScript + Preact workspace under `frontend/`

## 1.0.0 — 2026-07-17

Initial release.

### Highlights

- **HTML/CSS/JS views over Starfield** - an SFSE/CommonLibSF plugin that renders web UI in game via the Ultralight engine. Inspired by Prisma UI.
- **Mods surface** - press **F10** (rebindable in game) to open the unified Mods menu, where OSF UI and content mods expose their settings.
- **Keybinds view** - a visual keyboard map with inline rebinding and conflict badges.
- **Controller support** - the Mods and Keybinds surfaces are fully navigable
  with a gamepad: D-pad / left stick moves focus, A activates, B closes,
  right stick scrolls, LB/RB switch mods. The same layer makes both surfaces
  arrow-key navigable from the keyboard.

### For view authors

- View packages under `views/<modId>/<viewName>/` with a `manifest.json`;
  multiple views can load and composite together (HUDs beneath open menus).
- `targetVersion` (view manifests AND settings schemas) — declare the OSF UI
  version a mod is authored against; when the installed OSF UI is older, the
  Mods surface shows a "needs update" badge by the version number naming the
  mod (advisory only — everything still loads best-effort).
- JS bridge `window.osfui`, **protocol 1.0** (stable): request envelope with
  `requestId` correlation, uniform `ui.result` outcomes, and raw gamepad
  events (experimental).
- Declarative **settings schemas** with persistence under
  `Documents\My Games\Starfield\OSFUI\settings\`, versioning + migration,
  input contexts, and unbound-key support.
- Shared UI kit under the `--osf-*` CSS namespace, plus TypeScript
  definitions (`sdk/osfui.d.ts`).
- Developer loop: `devMode` verbose logging and first-frame PNG dump,
  **F11** in-place view reload, schema hot-reload, and a browser dev harness.

### For plugin authors

- Native C++ API (`sdk/OSFUI_API.h`): register shipped views at runtime and
  handle commands from your views.

### Engine integration

- Views open as real engine menus: proper menu-stack admission, game pause
  while the overlay is open, and a hardware cursor.
- Shipped `OSFUI/config.json` is the developer/boot file; user-facing
  settings live in the in-game menu and survive updates.
- The default build has zero Ultralight footprint; the real renderer is an
  opt-in build (`xmake f --with_ultralight=true`).
