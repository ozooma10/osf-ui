# Papyrus API validation mod

Throwaway in-game test for the `OSFUI` Papyrus natives (`src/api/PapyrusApi.*`
+ `data/Scripts/Source/OSFUI.psc`). **Not shipped**: it deploys as its own
MO2 mod, never into `data/`, so neither `tools/package.ps1` archives nor the
xmake deploy can pick it up.

> ⚠ Do NOT stage these files into `MO2\mods\OSF UI\` (the xmake deploy
> target): `xmake install` MIRRORS every installfiles subtree, silently
> deleting files it didn't put there (proven 2026-07-17 — a schema staged
> into `SFSE\Plugins\OSFUI\settings\` was pruned on the next build). A
> separate mod folder is the only stable home; MO2's VFS merges it over
> `Data` exactly like any third-party mod shipping a settings drop-in.

## Files

| File | What | Deploys to (`MO2\mods\OSF UI Papyrus Test\`) |
| --- | --- | --- |
| `osfui.paptest.json` | drop-in settings schema: one of each core type + an `F8` test hotkey | `SFSE\Plugins\OSFUI\settings\` |
| `OSFUITest.psc` / `.pex` | console-driven test suite (global functions, no esp needed) | `Scripts\` |

Enable the **"OSF UI Papyrus Test"** mod in MO2 once (new folders need an
MO2 refresh — F5 — to appear).

## Build + deploy

```powershell
& "C:\Program Files (x86)\Steam\steamapps\common\Starfield\Tools\Papyrus Compiler\PapyrusCompiler.exe" `
    OSFUITest.psc `
    "-i=<repo>\tests\papyrus;<repo>\data\Scripts\Source;C:\Modding\Starfield\PapyrusSource" `
    "-o=<repo>\tests\papyrus" `
    "-f=C:\Modding\Starfield\PapyrusSource\Starfield_Papyrus_Flags.flg"
Copy-Item OSFUITest.pex      "C:\Modding\Starfield\MO2\mods\OSF UI Papyrus Test\Scripts\"
Copy-Item osfui.paptest.json "C:\Modding\Starfield\MO2\mods\OSF UI Papyrus Test\SFSE\Plugins\OSFUI\settings\"
```

(For log detail, enable Papyrus logging in `StarfieldCustom.ini`:
`[Papyrus]` `bEnableLogging=1` — the HUD notifications work without it.)

## Run

Launch via MO2 + SFSE, load a save, open the console:

```
cgf "OSFUITest.RunAll"
```

Expected, in order:

1. HUD: `OSFUITest: callbacks registered - press the test hotkey (default F8)`
2. ~3 seconds of silence (the suite waits on each commit), then
   HUD: **`OSFUITest: ALL CHECKS PASSED`**. Any failure shows its own
   `OSFUITest FAIL: <check>` notification.
3. Close the console, press **F8** in gameplay →
   HUD: `OSFUITest: hotkey fired (testHotkey)`.
4. F10 → Mod Settings → a **"Papyrus API Test"** card exists; rebind its
   hotkey to another key, press that → notification names the new key. While
   the overlay is open, F8 must NOT fire (suppression).
5. `OSF UI.log` (SFSE logs folder) shows `PapyrusApi: natives bound on script
   'OSFUI'` at boot, two `registered token` lines from step 1, and exactly one
   `Set osfui.paptest.mode refused (invalid-value)` WARN from the suite (the
   `bogus` enum; the out-of-range int/float writes are clamped and committed,
   not refused).
6. Save, load that save → `PapyrusApi: cleared 2 script registration(s)` +
   re-bind line in the log; F8 no longer notifies (session-scoped) until
   `cgf "OSFUITest.Hook"` re-registers.

The Papyrus log (`Documents\My Games\Starfield\Logs\Script\Papyrus.0.log`)
carries the full `[OSFUITest] PASS/FAIL` list plus one
`OnSettingChanged osfui.paptest.<key>` line per committed write/reset.

Re-running `RunAll`/`Hook` in one session adds duplicate callback
registrations (a globals-only script has no state to dedupe) — duplicate
change traces afterwards are expected, not a bug. `cgf "OSFUITest.Drop" <token>`
(tokens are printed to the Papyrus log) exercises `Unregister`.

## Cleanup

Disable/delete the "OSF UI Papyrus Test" mod in MO2; the
`settings\values\osfui.paptest.json` values file OSF UI writes (lands in MO2
Overwrite under the VFS) can go too.
