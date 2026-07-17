# OSF UI — Install & Troubleshooting (for players)

OSF UI is an **early but working** SFSE plugin that draws an
HTML/CSS/JS overlay over Starfield. Today it ships a schema-driven **settings
(MCM-style)** panel and is a foundation other UI mods can build on. It is
**not** a finished product — expect rough edges and read *Known limitations*
below before installing.

## Requirements

- **Starfield on Steam.** Xbox/Game Pass is **not** supported (SFSE is Steam-only).
- **SFSE** (Starfield Script Extender) matching your game version — https://sfse.silverlock.org/
- **Address Library for Starfield** with the `versionlib-<your build>.bin` for the
  game version you run (a common AIO Address Library mod provides this).
- **Windows** with a D3D12-capable GPU (any modern card).

OSF UI is pinned to the game build it was compiled against (currently
**1.16.244** via the Address Library). A game patch may require an updated
release — see the layout-guard note under *Troubleshooting*.

## Install

**Mod manager (recommended — MO2 or Vortex):** install the release archive like
any SFSE plugin and enable it. The payload is:

```
SFSE/Plugins/
  OSFUI.dll
  OSFUI/            (config + views + the Ultralight runtime)
```

**Manual:** extract that `SFSE/Plugins/...` tree into your Starfield `Data` folder.

Launch the game **through SFSE** (`sfse_loader.exe` or your mod manager's SFSE
entry). The base game launcher will not load the plugin.

## First run / verifying it works

1. In game, press **F10** — the **Mods** menu should appear; F10 again hides
   it, `Esc` closes it.
2. While it's open, the game is input-frozen and the real Windows pointer
   appears (hardware cursor — no lag, changes shape over buttons/text) to drive
   the UI; changes save automatically.
3. The left rail lists every installed mod (OSF UI itself first). Selecting a
   mod shows its settings; mods that register panels or HUDs get launch
   buttons / toggles at the top of their page. **Keybinds** (a visual keyboard
   map for rebinding) opens from the OSF UI entry.
4. The log is written to:
   `Documents\My Games\Starfield\SFSE\Logs\OSF UI.log`
   (Documents may be OneDrive-redirected.)

A **"MOD MENUS"** entry is also injected into the game's pause menu and opens
the overlay. If you have a controller, the menu and its panels are navigable
with the D-pad and face buttons.

## Where are my settings?

Everything user-facing is in the in-game menu (**F10 → OSF UI**): the
open/close key, the pause-menu entry, and game-key collision warnings.
(Gameplay controls — including gamepad — always freeze while a menu captures
input; there is no setting for it. To use the game console, close the overlay
first — its key is swallowed while the overlay is open.)
Choices persist to `Documents\My Games\Starfield\OSFUI\settings\` (one JSON
per mod) and survive mod updates.

`SFSE/Plugins/OSFUI/config.json` is a **developer/boot file** (renderer
backends, diagnostic switches). It is overwritten when the mod updates, so
don't keep personal edits there — unknown keys are ignored with a warning
in the log.

**Fixing the game-key table:** the Keybinds view's "Starfield (…)" rows come
from a curated table (`OSFUI/vanillakeys.json`) plus your in-game rebinds. If
a row is wrong or missing after a game patch, create
`Documents\My Games\Starfield\OSFUI\vanillakeys.user.json` — it overlays the
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

Open `OSF UI.log` first — it states what happened.

| Symptom | Likely cause / fix |
|---|---|
| F10 does nothing, log absent | Game wasn't launched via SFSE, or SFSE doesn't match the game version. |
| Log says `UI layout guard FAILED` | The game updated and the plugin's data is stale for this build. **Don't play with it enabled** — wait for an updated release (or a matching Address Library / CommonLibSF). This guard is deliberate: it refuses rather than risk corruption. |
| Overlay never appears, log shows renderer/compositor warnings | The Ultralight runtime DLLs or the located game device weren't available; the overlay disables itself loudly. Re-install the archive intact. |
| Overlay appears but is blank | The renderer fell back to `null` (missing `ultralight/` runtime files) — re-install. |
| Overlay lingers oddly during a load | It should auto-hide on loading screens / main menu; if not, F10 to hide and report the log. |
| Overlay never appears (or vanishes) while running ReShade / RTSS / Steam overlay / frame-gen tools | Hook-chain ordering. Check the log for `Present slot 8 owner before hook` (which tool hooked first) and `no present has reached our hook` (a tool re-hooked over OSF UI without chaining). Try changing the injection/load order of the overlay tools and report the log lines + your overlay stack. |
| No pointer while the overlay is open, or the pointer flickers/jumps to center | Something (the engine or another overlay) is fighting the hardware cursor. The log shows `HardwareCursor: activated/deactivated` pairs on F10. Report it; as a stopgap `"hardwareCursor": false` in `config.json` restores the old input path — but it has no visible pointer, so it is a diagnostic, not a fix. |

**Safe-disable without uninstalling:** set `"enabled": false` in
`SFSE/Plugins/OSFUI/config.json` (or disable the mod in your manager).

## Uninstall

- Disable/remove the mod in MO2/Vortex, **or** delete `OSFUI.dll` and the
  `OSFUI/` folder from `Data/SFSE/Plugins/`.
- Saved settings live in `Documents\My Games\Starfield\OSFUI\` — delete
  that folder to remove them too. OSF UI writes nothing into your save files.

## Known limitations (v0.x)

- **Steam only** (SFSE limitation).
- **HDR / 10-bit output is not supported yet** — the overlay detects an
  HDR/10-bit backbuffer, logs a warning naming the format, and deliberately
  does **not** draw on it (drawing would produce wrong colors). Symptom: the
  overlay never appears and `OSF UI.log` has a `cannot render correctly into
  it yet` line. Workaround: run Starfield in SDR. Full HDR output is on the
  roadmap.
- **Untested display/overlay setups** — frame generation (DLSS-G / FSR-FG)
  and overlay tools (ReShade, Steam overlay, RTSS) have **not** been
  validated. On those the overlay may not appear or may draw on the wrong
  output. OSF UI chains politely after tools that hooked first and logs
  diagnostics for the broken cases (see the troubleshooting table above).
  Reports welcome.
- **Tied to a game build** via the Address Library; a patch can require an update.
- **Input:** text entry follows your OS keyboard layout (dead keys/AltGr
  work), but IME composition (e.g. CJK input) is not supported yet; gamepad
  navigation is basic (D-pad/sticks/A/B) and being refined.
- **For UI authors:** the `window.osfui` bridge API is version **0.x and
  unstable** — see [authoring-views.md](authoring-views.md). It may change between
  releases until it reaches 1.0.

## Reporting issues

Include your `OSF UI.log`, game build, SFSE version, and whether you run
HDR / frame-gen / ReShade / overlays. Issues: https://github.com/ozooma10/osf-ui
