# __OSFUI_DISPLAY_NAME__

A settings-only OSF UI mod: a settings page and a rebindable hotkey.

- `mod/SFSE/Plugins/OSFUI/settings/__OSFUI_MOD_ID__.json` - the settings page config
- `mod/Scripts/Source/__OSFUI_SCRIPT_NAME__.psc` - the hotkey handler
- `tools/papyrus/OSFUI.psc` - the OSF UI compiler API (not shipped)

## Build and deploy

Install the **Starfield Creation Kit** through Steam (Library > Tools), then:

```powershell
./build-deploy.ps1 -Mo2Mods "C:\path\to\MO2\mods"
```

Standard Steam install paths are found automatically. For a portable or nonstandard install, pass `-StarfieldRoot`, `-PapyrusCompiler`, or `-PapyrusSource`. 
Without `-Mo2Mods` the script only compiles.

## Verify in game

1. Refresh MO2 (F5) and enable the mod.
2. Load a save, then press **F10** and open **__OSFUI_DISPLAY_NAME__**.
3. Close every menu and press **F8**. A notification appears.
s
Hotkeys are dropped while a game menu or the console is open, so the press only works during gameplay. 
Rebind the key in the menu and the new key works immediately.

Then **save, reload, and press it again**. This is what `onPress` buys you:
the target is read from the schema at delivery time, so unlike `OSFUI.RegisterForHotkey` there is no registration to lose and no `OnPlayerLoadGame` hook to write.

## Edit it

- **Settings page** - edit the JSON. Rows support `bool`, `int`, `float`, `enum`, `flags`, `string`, and `key` types. Read values back with `OSFUI.GetBool` / `GetInt` / `GetFloat` / `GetString`. 
  See [authoring-settings.md](https://github.com/ozooma10/osf-ui/blob/main/docs/authoring-settings.md).
- **Hotkey** - a `"type": "key"` row with an `onPress` target. Its `script` must match the `ScriptName` exactly and the function must be GLOBAL with an exact `(string, string)` signature. Stored key names identify physical positions; OSF UI shows the player's current keyboard-layout label.
- **Add a view later** - run `npm create osfui@latest` and pick the menu or HUD view; the schema and script here move across unchanged.

## Ship it

Zip the contents of `mod/` (so `SFSE` and `Scripts` sit at the archive root) and upload. OSF UI is the only dependency; if it is missing, every `OSFUI.*` call fails soft and returns the default you passed.
