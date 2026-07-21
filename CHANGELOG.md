# Changelog

## Unreleased

### Fixed

- With a menu open, gamepad input no longer leaks into the game underneath: the thumbsticks walked the player around (and buttons could trigger game actions) behind the overlay, because the engine's control-disable flags don't gate thumbstick movement. Gamepad events are now consumed at the overlay's input receiver while a capturing menu is open, so the game never sees them; views still receive them normally (default navigation mapping and raw `ui.gamepad` alike).

### Other changes

- If the Microsoft Edge WebView2 Runtime is not installed, a dialog now appears at game launch naming the problem and offering to open Microsoft's installer download — previously the overlay just never appeared, with the cause buried in `OSF UI.log`.

## 1.1.2 — 2026-07-21

### Fixed

- Controller support works again. The WebView2 renderer kept Windows keyboard focus in the browser for the whole overlay session, and Windows only delivers gamepad input to the process whose window has focus — so the game engine (and the overlay with it) went controller-deaf. The overlay now leaves focus with the game and only moves it into the browser while you actually type in a text field (click a field or start typing); controller navigation resumes the moment text entry ends.
- Fixed a crash tied to the pause-menu entry while the menu list was rebuilding.
- Try to fix crashes when Frame Generation enabled.


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
