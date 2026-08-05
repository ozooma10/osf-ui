# OSF UI — Install & Troubleshooting (for players)

OSF UI is an SFSE plugin that draws an HTML/CSS/JS overlay over Starfield. It ships a schema-driven settings (MCM-style) panel and is the foundation other UI mods build on. It's early software — read *Known limitations* before installing.

## Requirements

- Starfield on **Steam**. Xbox/Game Pass isn't supported (SFSE is Steam-only).
- SFSE matching your game version — https://sfse.silverlock.org/
- Address Library for Starfield with the `versionlib-<your build>.bin` for your version (the common AIO Address Library mod provides it).
- Microsoft Edge WebView2 Runtime (Evergreen). Preinstalled on Windows 11 and most Windows 10 machines; if missing, OSF UI shows a launch dialog with a download link (https://go.microsoft.com/fwlink/p/?LinkId=2124703 — install, restart the game).
- Windows with a D3D12-capable GPU.

OSF UI is pinned to the game build it was compiled against (currently **1.16.244**, via the Address Library). A game patch may require an updated release — see the layout-guard row below.

## Install

Mod manager (MO2 or Vortex, recommended): install the release archive like any other SFSE plugin and enable it. Payload:

```
SFSE/Plugins/
  OSFUI.dll
  OSFUI/            (config + views)
    bin/osfui_webview2_host.exe
```

Manual: extract that `SFSE/Plugins/...` tree into your Starfield `Data` folder.

Launch through SFSE (`sfse_loader.exe` or your manager's SFSE entry) — the base launcher doesn't load SFSE plugins.

## First run

1. Press **F10**. The Mods menu appears; F10 hides it, `Esc` closes it.
2. While it's open the game is input-frozen and the normal Windows pointer appears (a hardware cursor: no lag, changes shape over buttons and text). Changes save automatically.
3. The left rail lists every installed mod, OSF UI first. Mods that register panels or HUDs get launch buttons and toggles at the top of their page. Keybinds (a visual keyboard map) opens from the OSF UI entry.
4. The log is `Documents\My Games\Starfield\SFSE\Logs\OSF UI.log` (Documents may be OneDrive-redirected).

A "MOD MENUS" pause-menu entry opens the same overlay. Controllers navigate with the D-pad and face buttons.

## Where are my settings?

Everything user-facing is in the in-game menu (F10 → OSF UI): open/close key, language, the pause-menu entry, game-key collision warnings, and under *Diagnostics* **Bug reporting** — turn that off and neither the System Health reporter nor the post-crash prompt offers to send anything. Third-party developer views appear in the mod menu only when `devMode` is on in `config.json`; the settings hub shows an amber **DEV MODE** tag while it is.

Gameplay controls, gamepad included, always freeze while a menu captures input; there's no setting for it. To use the game console, close the overlay first — the console key is swallowed while it's open.

Choices persist to `Data\SFSE\Plugins\OSFUI\settings\values\` (one JSON file per mod) and survive updates. Under MO2 that write goes through the VFS, so look in Overwrite or whichever mod claims the path — which also makes settings per-profile and part of instance backups.

`SFSE/Plugins/OSFUI/config.json` is a developer/boot file for diagnostic and input switches. It's overwritten on update, so don't keep personal edits there. Unknown keys are ignored with a log warning.

**A mod is missing, or a warning sits atop the Mods rail:** a settings file that fails to load always produces a warning naming the file and reason (bad filename, JSON parse error with line/column, corrupt saved values). A corrupt values file is renamed `<mod>.json.bad` and defaults are used; if you were hand-editing, fix the `.bad` file and rename it back. Same details in `OSF UI.log`.

**Keys and keyboard layouts:** stored key names (`"F8"`, `"Semicolon"`) identify *physical positions* on the US reference keyboard — the same convention the game's own controlmap uses — so a binding means the same key on any layout, and the binding UI shows what your layout actually prints there (a German layout shows `Ö` on the semicolon-position key, and the ISO `<>` key is bindable). Bindings saved by pre-2.x versions are migrated once, using the layout active the first time this version loads; if you bound keys under a *different* layout than the one active then, re-check those bindings once. Don't downgrade to a pre-2.x version after the migration ran — a rebind made there won't re-migrate. Switching layouts while the overlay is open updates the displayed keycaps the next time the game window has focus.

**The game-key table is unavailable:** OSF UI now reads Starfield's live `ControlMap`; it does not load `vanillakeys.json`, `ControlMap.txt`, or `vanillakeys.user.json`. On an unsupported game build or a failed layout safety gate, System Health reports **Starfield's key map is unavailable**, vanilla rows/warnings are disabled, and mode-scoped mod hotkeys fail closed. Update OSF UI for that exact Starfield version. Existing `Documents\My Games\Starfield\OSFUI\vanillakeys.user.json` files are left untouched but ignored and may be removed manually.

## Troubleshooting

Check `OSF UI.log` first.

| Symptom | Likely cause / fix |
|---|---|
| F10 does nothing, no log file | Not launched through SFSE, or SFSE doesn't match the game version. |
| Log says `UI layout guard FAILED` | The game updated and the plugin's data is stale for this build. Don't play with it enabled; wait for an updated release (or a matching Address Library / CommonLibSF). Intentional: the plugin disables itself rather than patch the wrong offsets. |
| "WebView2 Runtime missing" dialog at launch | Install the Evergreen runtime (https://go.microsoft.com/fwlink/p/?LinkId=2124703), restart the game. No mod reinstall needed. |
| Overlay never appears, renderer/compositor warnings in log | The WebView2 Runtime, host executable, or the game's device wasn't available, so the overlay disabled itself. Install the runtime and re-install the archive intact. |
| Overlay appears but is blank | Check the log for WebView2 host launch, pipe, navigation or shared-texture errors, then verify `OSFUI/bin/osfui_webview2_host.exe` exists. |
| One mod's page or HUD is blank and System Health shows a compatibility warning naming that view | That view targets the OSF UI 1.x mod API, which 2.0 removed. It loads, but every bridge call fails. Nothing to configure — the mod needs an author update. The card names the view and the version it was authored against. |
| A mod's OSF UI features vanished after updating, and System Health names a `.dll` | That mod's SFSE plugin asked for an OSF UI native API major this build doesn't provide, so it gets no bridge at all — deliberately, since a half-working plugin is worse than an absent one. (Plugins built against any 1.x version still connect normally.) The rest of that mod usually keeps working. |
| Overlay lingers during a load | It should auto-hide on loading screens and the main menu. If not, hide with F10 and report the log. |
| Overlay never appears (or vanishes) with ReShade / RTSS / Steam overlay / frame-gen tools | Current builds don't join the DXGI Present hook chain. Check the log for `seam-only overlay armed`, `shared ring adopted`, `FIRST SEAM OVERLAY DRAW`; report the missing stage and your overlay stack. No injection/load-order workaround should be needed. |
| Crash opening the overlay with BetterConsole installed | Fixed: current builds never create a probe swapchain or hook Present. Update OSF UI and confirm the log has `seam-only overlay armed`. |
| Crash opening the overlay with OptiScaler + Steam overlay | Fixed: current builds always composite through the Scaleform seam and never extend the wrapped Present chain. All three can stay enabled; confirm `seam-only overlay armed`. |
| No pointer while the overlay is open, or it flickers/jumps to center | The engine or another overlay is fighting the hardware cursor; the log shows `HardwareCursor: activated/deactivated` pairs on F10. Report it. `"hardwareCursor": false` in `config.json` restores the old input path, but that path has no visible pointer — a diagnostic, not a fix. |
| *(developers)* I edited a deployed built-in view's `main.js` / `style.css` and nothing changed | That's generated output. Built-in views are generated from `frontend/src/` into ignored `build/frontend/views/` and redeployed by xmake. Edit `frontend/src/` and run `xmake build`; a loaded view reloads automatically in `devMode`. See [../frontend/README.md](../frontend/README.md). Your own mod's view is unaffected — third-party views are hand-authored and load as-is. |

To disable without uninstalling: `"enabled": false` in `SFSE/Plugins/OSFUI/config.json`, or disable the mod in your manager.

## Debugging your own view (for authors)

Your view is a real Chromium document, so the debugger is the one you know. OSF UI's contribution: **every failure you can cause prints to that view's own console** with an `[osfui]` prefix, so DevTools shows it with full object inspection.

- **F12 opens DevTools** while your view is the focused menu. Needs developer tooling on — `"devMode": true` in `config.json`, or (better, temporary) `osfui dev --game` from your project, which writes an expiring author-mode marker. F12 targets the focused menu; debug a background HUD from the browser harness ([view-toolchain.md](view-toolchain.md)).
- **Read the `[osfui]` errors.** A rejected request prints `[osfui] request "<name>" failed: <code> — <message>` with the rejecting payload attached, before any `Uncaught (in promise)` noise. Same prefix covers a missing bridge, a client timeout, and an exception your own event or state handler threw.
- **Mistakes the page couldn't otherwise hear about come back to it.** Sending to a request endpoint, naming a nonexistent endpoint, a malformed envelope, a backend that never answered — each arrives as a dev-only `osfui.debug.error` event and prints as `[osfui] host rejected <code>: <message>`. These exist only while developer tooling is on; in a player's game, repeated misuse from one view raises a System Health card instead.
- **Trace the traffic** when the question is what actually crossed the bridge: `localStorage["osfui:trace"] = "1"` in the view's console, then reload. Every envelope both directions is logged via `console.debug` — kind, name, id, payload, reply latency. It answers the blank-HUD question directly: either your state key arrives at boot (your view's bug) or it doesn't (your backend's).
- **It all lands in `OSF UI.log` too** while developer tooling is on: `console.error` → ERROR, `console.warn` → WARN, everything else → DEBUG, as `Runtime: view '<id>' console: …`. Keep the trace flag off when you're not reading it — it turns the log into a full bridge capture. The native side of the same failures is logged regardless, tagged `[content]`; see [logging.md](logging.md).

## Uninstall

- Disable or remove the mod in MO2/Vortex, or delete `OSFUI.dll` and the `OSFUI/` folder from `Data/SFSE/Plugins/`.
- Saved settings live in `Data\SFSE\Plugins\OSFUI\settings\values\` (under MO2: Overwrite, or wherever you sorted that folder) and go with the mod's files. Legacy `Documents\My Games\Starfield\OSFUI\vanillakeys.user.json` files are no longer read. OSF UI writes nothing into your saves.

## Known limitations

- Steam only (SFSE limitation).
- Frame Generation is supported: the overlay draws inside Starfield's own Scaleform UI pass, so FSR3-FG and DLSS-G pace it like native UI. Validated against built-in FSR3 FG; the wider matrix (OptiScaler/Nukem FG, OptiScaler + Steam overlay, display-mode and resolution changes) is still being worked through — reports welcome.
- HDR / 10-bit output is no longer blocked. The overlay renders through the engine's UI buffer and never inspects the swapchain format. Color handling on HDR output is untuned — treat the result as unvalidated, not correct.
- Other overlay tools (ReShade, Steam overlay, RTSS) are no longer a load-order problem: OSF UI installs no `Present` hook. Broken combinations still log the diagnostics above.
- Tied to a game build via the Address Library; a patch can require an update.
- Text entry follows your OS keyboard layout (dead keys and AltGr work), but IME composition (e.g. CJK) isn't supported yet. Key *bindings* are physical and layout-independent, and the binding UI shows your layout's keycaps (see "Keys and keyboard layouts" above). Gamepad navigation is basic (D-pad/sticks/A/B) and being refined.
- For UI authors: the `window.osfui` bridge protocol is **2.0**, and 2.0 is a break — a view written for 1.x needs an author update. (Native SFSE plugins are unaffected: their C ABI is versioned separately and stays additive at 1.8.) The 1.x surface had grown aliases and implicit behaviors that couldn't be fixed compatibly; the failure is at least legible (a card naming the view or DLL) rather than a blank page. From here, additive changes bump the minor and breaking changes the major — declare `targetVersion`, see [authoring-views.md](authoring-views.md).

## Reporting issues

Open **Mod Settings → System Health → Report a bug**. When automatic reporting is configured, OSF UI shows the exact files and retention period before upload, requires consent, redacts known account and installation roots locally, and uploads bounded tails of `OSF UI.log` and `OSF UI.webview2-host.log` with the current health snapshot. Attachments are stored privately for 30 days; after server-side abuse checks a public GitHub issue may be created carrying only your title, description, reproduction steps and an opaque report reference. Acceptance returns a private reference immediately, even while issue publication is queued or paused.

If Starfield exits non-zero while OSF UI is active or opening, the surviving primary WebView2 helper offers the same upload in a Windows Yes/No dialog, without claiming OSF UI caused the exit. It inspects no external crash logs before consent. After **Yes**, it checks only the standard `SFSE/Crashlogs` folder for the newest Trainwreck/Crash Logger report from that session; if present it's uploaded privately with a path-free attachment name, local redaction and a bounded tail. When an OSF Animation surface was active, the dialog instead names the OSF Animation repository and its `OSF Animation.log`, and the public issue routes to `ozooma10/osf-animation`.

Reporting can be turned off entirely in **OSF UI → Diagnostics → Bug reporting**; the crash prompt honors that from the next launch.

If reporting is disabled or fails, use **Copy diagnostic report** and **Open log folder**, then attach the result at https://github.com/ozooma10/osf-ui/issues.
