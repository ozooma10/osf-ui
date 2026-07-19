# OSF UI — Install & Troubleshooting (for players)

OSF UI is an SFSE plugin that draws an HTML/CSS/JS overlay over Starfield.
Right now it ships a schema-driven settings (MCM-style) panel and serves as a
foundation for other UI mods to build on. It's early software with rough
edges; read *Known limitations* before installing.

## Requirements

- Starfield on Steam. Xbox/Game Pass is not supported (SFSE is Steam-only).
- SFSE (Starfield Script Extender) matching your game version —
  https://sfse.silverlock.org/
- Address Library for Starfield with the `versionlib-<your build>.bin` for
  your game version (the common AIO Address Library mod provides this).
- Windows with a D3D12-capable GPU (any modern card).

OSF UI is pinned to the game build it was compiled against (currently
**1.16.244** via the Address Library). A game patch may require an updated
release; see the layout-guard entry under *Troubleshooting*.

## Install

With a mod manager (MO2 or Vortex, recommended): install the release archive
like any other SFSE plugin and enable it. The payload is:

```
SFSE/Plugins/
  OSFUI.dll
  OSFUI/            (config + views + the Ultralight runtime)
```

Manual: extract that `SFSE/Plugins/...` tree into your Starfield `Data`
folder.

Launch the game through SFSE (`sfse_loader.exe` or your mod manager's SFSE
entry). The base game launcher does not load SFSE plugins.

## First run / verifying it works

1. In game, press **F10**. The Mods menu should appear; F10 again hides it,
   `Esc` closes it.
2. While the menu is open the game is input-frozen and the normal Windows
   pointer appears (a hardware cursor, so no lag, and it changes shape over
   buttons and text). Changes save automatically.
3. The left rail lists every installed mod, with OSF UI itself first.
   Selecting a mod shows its settings. Mods that register panels or HUDs get
   launch buttons and toggles at the top of their page. Keybinds (a visual
   keyboard map for rebinding) opens from the OSF UI entry.
4. The log is written to
   `Documents\My Games\Starfield\SFSE\Logs\OSF UI.log`
   (Documents may be OneDrive-redirected).

A "MOD MENUS" entry is also added to the game's pause menu and opens the same
overlay. On a controller, the menu and its panels can be navigated with the
D-pad and face buttons.

## Where are my settings?

Everything user-facing is in the in-game menu (F10 → OSF UI): the open/close
key, the pause-menu entry, and game-key collision warnings. Gameplay controls,
including gamepad, always freeze while a menu captures input; there is no
setting for this. To use the game console, close the overlay first — the
console key is swallowed while the overlay is open.

Choices persist to `Data\SFSE\Plugins\OSFUI\settings\values\` (one JSON file
per mod) and survive mod updates. Under MO2 that write goes through the VFS,
so look in Overwrite or whichever mod claims the path. This also makes your
settings per-profile and part of instance backups.

`SFSE/Plugins/OSFUI/config.json` is a developer/boot file (renderer backends,
diagnostic switches). It gets overwritten when the mod updates, so don't keep
personal edits there. Unknown keys are ignored with a warning in the log.

**A mod is missing from the list, or a warning sits at the top of the Mods
rail:** a settings file that fails to load always produces a warning in the
rail naming the file and the reason (bad filename, JSON parse error with
line/column, corrupt saved values). A corrupt values file is renamed to
`<mod>.json.bad` and defaults are used instead; if you were hand-editing it,
fix the `.bad` file and rename it back. The same details appear in
`OSF UI.log`.

**Fixing the game-key table:** the Keybinds view's "Starfield (…)" rows come
from a curated table (`OSFUI/vanillakeys.json`) plus your in-game rebinds. If
a row is wrong or missing after a game patch, create
`Documents\My Games\Starfield\OSFUI\vanillakeys.user.json`. It overlays the
shipped table and survives updates:

```json
{
  "formatVersion": 1,
  "add":      [ { "event": "NewEvent", "label": "New thing", "key": "K" } ],
  "replace":  [ { "event": "QuickSave", "key": "F6" } ],
  "suppress": [ "Powers" ]
}
```

## Troubleshooting

Check `OSF UI.log` first.

| Symptom | Likely cause / fix |
|---|---|
| F10 does nothing, no log file | The game wasn't launched through SFSE, or SFSE doesn't match the game version. |
| Log says `UI layout guard FAILED` | The game updated and the plugin's data is stale for this build. Don't play with it enabled; wait for an updated release (or a matching Address Library / CommonLibSF). This is intentional: the plugin disables itself rather than patch the wrong offsets. |
| Overlay never appears, renderer/compositor warnings in log | The Ultralight runtime DLLs or the game's device weren't available, so the overlay disabled itself. Re-install the archive intact. |
| Overlay appears but is blank | The renderer fell back to `null` because `ultralight/` runtime files are missing. Re-install. |
| Overlay lingers oddly during a load | It should auto-hide on loading screens and the main menu. If it doesn't, hide it with F10 and report the log. |
| Overlay never appears (or vanishes) while running ReShade / RTSS / Steam overlay / frame-gen tools | Hook-chain ordering. Check the log for `Present slot 8 owner before hook` (says which tool hooked first) and `no present has reached our hook` (a tool re-hooked over OSF UI without chaining). Try changing the injection/load order of your overlay tools, and include the log lines and your overlay stack in a report. |
| No pointer while the overlay is open, or the pointer flickers/jumps to center | The engine or another overlay is fighting the hardware cursor. The log shows `HardwareCursor: activated/deactivated` pairs on F10. Report it. As a stopgap, `"hardwareCursor": false` in `config.json` restores the old input path, but that path has no visible pointer, so treat it as a diagnostic, not a fix. |
| *(developers)* I edited a built-in view's `main.js` / `style.css` and nothing changed | You edited generated output. Everything under `data/OSFUI/views/` is built from `frontend/src/` — the game loaded exactly what is on disk, and your edit will be destroyed by the next build. Edit `frontend/src/`, run `npm --prefix frontend run build`, redeploy, then F11 (with `devMode`). See [../frontend/README.md](../frontend/README.md). Your *own* mod's view is unaffected: third-party views are hand-authored and load as-is. |

To disable without uninstalling: set `"enabled": false` in
`SFSE/Plugins/OSFUI/config.json`, or disable the mod in your manager.

## Uninstall

- Disable or remove the mod in MO2/Vortex, or delete `OSFUI.dll` and the
  `OSFUI/` folder from `Data/SFSE/Plugins/`.
- Saved settings live in `Data\SFSE\Plugins\OSFUI\settings\values\` (under
  MO2: Overwrite, or wherever you sorted that folder); removing the mod's
  files removes them too. If you created
  `Documents\My Games\Starfield\OSFUI\vanillakeys.user.json`, delete that
  folder as well. OSF UI writes nothing into your save files.

## Known limitations

- Steam only (SFSE limitation).
- No HDR / 10-bit output yet. The overlay detects an HDR/10-bit backbuffer,
  logs a warning naming the format, and doesn't draw on it (the colors would
  be wrong). Symptom: the overlay never appears and `OSF UI.log` has a
  `cannot render correctly into it yet` line. Workaround: run Starfield in
  SDR. HDR output is planned.
- Untested display/overlay setups: frame generation (DLSS-G / FSR-FG) and
  overlay tools (ReShade, Steam overlay, RTSS) haven't been validated. With
  those the overlay may not appear or may draw on the wrong output. OSF UI
  chains after tools that hooked first and logs diagnostics for the broken
  cases (see the table above). Reports welcome.
- Tied to a game build via the Address Library; a patch can require an
  update.
- Input: text entry follows your OS keyboard layout (dead keys and AltGr
  work), but IME composition (e.g. CJK input) isn't supported yet. Gamepad
  navigation is basic (D-pad/sticks/A/B) and being refined.
- For UI authors: the `window.osfui` bridge protocol is 1.0 and stable.
  Additive changes bump the minor version, breaking changes the major.
  Declare the version you authored against with `targetVersion` — see
  [authoring-views.md](authoring-views.md).

## Reporting issues

Include your `OSF UI.log`, game build, SFSE version, and whether you run
HDR, frame generation, ReShade, or other overlays.
Issues: https://github.com/ozooma10/osf-ui
