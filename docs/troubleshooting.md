# OSF UI — Install & Troubleshooting (for players)

OSF UI (StarfieldWebUI) is an **early but working** SFSE plugin that draws an
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
  OSF UI.dll
  StarfieldWebUI/            (config + views + the Ultralight runtime)
```

**Manual:** extract that `SFSE/Plugins/...` tree into your Starfield `Data` folder.

Launch the game **through SFSE** (`sfse_loader.exe` or your mod manager's SFSE
entry). The base game launcher will not load the plugin.

## First run / verifying it works

1. In game, press **F10** — the overlay (settings panel) should appear; F10 again
   hides it. `Esc` closes it.
2. While it's open, the game is input-frozen and the mouse drives a cursor in the
   panel; settings changes save automatically.
3. The log is written to:
   `Documents\My Games\Starfield\SFSE\Logs\StarfieldWebUI.log`
   (Documents may be OneDrive-redirected.)

Optional: the shipped config also loads a small demo **HUD** layer (top-right,
shows the in-game date/time) to showcase multi-view. Press **Tab** to move focus
between interactive panels.

## Troubleshooting

Open `StarfieldWebUI.log` first — it states what happened.

| Symptom | Likely cause / fix |
|---|---|
| F10 does nothing, log absent | Game wasn't launched via SFSE, or SFSE doesn't match the game version. |
| Log says `UI layout guard FAILED` | The game updated and the plugin's data is stale for this build. **Don't play with it enabled** — wait for an updated release (or a matching Address Library / CommonLibSF). This guard is deliberate: it refuses rather than risk corruption. |
| Overlay never appears, log shows renderer/compositor warnings | The Ultralight runtime DLLs or the located game device weren't available; the overlay disables itself loudly. Re-install the archive intact. |
| Overlay appears but is blank | The renderer fell back to `null` (missing `ultralight/` runtime files) — re-install. |
| Overlay lingers oddly during a load | It should auto-hide on loading screens / main menu; if not, F10 to hide and report the log. |

**Safe-disable without uninstalling:** set `"enabled": false` in
`SFSE/Plugins/StarfieldWebUI/config.json` (or disable the mod in your manager).

## Uninstall

- Disable/remove the mod in MO2/Vortex, **or** delete `OSF UI.dll` and the
  `StarfieldWebUI/` folder from `Data/SFSE/Plugins/`.
- Saved settings live in `Documents\My Games\Starfield\StarfieldWebUI\` — delete
  that folder to remove them too. OSF UI writes nothing into your save files.

## Known limitations (v0.x)

- **Steam only** (SFSE limitation).
- **Untested display/overlay setups** — HDR / 10-bit output, frame generation
  (DLSS-G / FSR-FG), and overlay tools (ReShade, Steam overlay, RTSS) have **not**
  been validated. On those the overlay may not appear or may draw on the wrong
  output. Reports welcome.
- **Tied to a game build** via the Address Library; a patch can require an update.
- **Input:** keyboard routing is US-layout only; no gamepad/controller navigation
  yet; with two interactive panels, `Tab` switches panels (so in-panel `Tab`
  field navigation is overridden in that mode).
- **For UI authors:** the `window.starfield` bridge API is version **0.x and
  unstable** — see [authoring-views.md](authoring-views.md). It may change between
  releases until it reaches 1.0.

## Reporting issues

Include your `StarfieldWebUI.log`, game build, SFSE version, and whether you run
HDR / frame-gen / ReShade / overlays. Issues: https://github.com/ozooma10/osf-ui
