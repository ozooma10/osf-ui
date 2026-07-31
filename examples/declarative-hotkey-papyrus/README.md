# Example — declarative Papyrus hotkey

This is a minimal installable mod for testing a schema-owned `onPress` target.
It contains no `.esp`/`.esm`, no startup quest, and no callback registration:
OSF UI reads the target from the settings schema and queues the GLOBAL Papyrus
function when the configured key is pressed.

## Build and deploy

From PowerShell:

```powershell
.\examples\declarative-hotkey-papyrus\build-deploy.ps1
```

The script compiles `OSFUIHotkeySample.psc` and copies the deployable `mod/`
tree to:

```text
C:\Modding\Starfield\MO2\mods\OSF UI Declarative Hotkey Sample
```

The compiler, game Papyrus sources, and MO2 paths can be overridden:

```powershell
.\examples\declarative-hotkey-papyrus\build-deploy.ps1 `
  -PapyrusCompiler "D:\Steam\steamapps\common\Starfield\Tools\Papyrus Compiler\PapyrusCompiler.exe" `
  -PapyrusSource "D:\StarfieldModding\PapyrusSource" `
  -Mo2Mods "D:\MO2\mods"
```

Refresh MO2 with F5 and enable **OSF UI Declarative Hotkey Sample** in the
left pane. There is no plugin to enable in the right pane.

## Test

1. Launch Starfield through MO2 + SFSE and load a save.
2. Close the console and all menus, then press F8.
3. Expect `OSF UI onPress: osfui.hotkey-sample.showNotification` as a HUD
   notification and in the Papyrus log.
4. Open F10 → Mod Settings → Declarative Hotkey Sample, rebind the key, close
   the menu, and verify the new key fires while F8 no longer does.
5. Save and load without running any registration function. The key must still
   fire. Pressing it while a menu, console, overlay capture, or key rebind is
   active must not fire.

To test diagnostics, change `onPress.script` in the deployed JSON to
`OSFUIHotkeySampleMissing`, restart the game, and press the key. The ordinary
hotkey still exists, while System Health should report
`settings.hotkey-target:osfui.hotkey-sample.showNotification` with the missing
target details. Restore the script name afterwards.

## Adapt it to start a quest

Replace the notification body in `OnHotkey` with a guarded, plugin-local form
lookup:

```papyrus
Quest target = Game.GetFormFromFile(0x000800, "YourMod.esm") as Quest
If target != None
    target.Start()
EndIf
```

`0x000800` is the quest record's plugin-local FormID, not its load-order
dependent runtime FormID.

The schema already declares `"targetVersion": "2.0.0"`, the release that first
ships `onPress`. Keep that in a real mod: an older OSF UI still shows and
rebinds the key, it just dispatches nothing when the key is pressed — the
**needs update** badge is the only thing that tells the player why.

## Why this needs no registration

`onPress` is schema-owned and immutable, so the dispatch target survives a save
load with no `OnPlayerLoadGame` re-registration — unlike
`OSFUI.RegisterForHotkey`, whose token is session-scoped like every other
Papyrus event registration. That is the whole point of the declarative form:
there is nothing to keep alive, so there is nothing to lose.

It is an extra channel, not a replacement. One press still notifies native
subscribers and the `ui.hotkey` web event, then any registered Papyrus
callback, and `onPress` last — a mod that does both gets both.

## Files

| File | Deployed location |
| --- | --- |
| `mod/Scripts/OSFUIHotkeySample.pex` | `Data/Scripts/` |
| `mod/Scripts/Source/OSFUIHotkeySample.psc` | optional source alongside the PEX |
| `mod/SFSE/Plugins/OSFUI/settings/osfui.hotkey-sample.json` | OSF UI settings schema |
