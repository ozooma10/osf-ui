# Authoring settings

One JSON file gives your mod a page in Mod Settings with validated, persistent controls.

The fastest way to start is:

```powershell
npm create osfui@latest -- --surface settings
```

This creates a settings schema, a Papyrus hotkey example, and a build script. To add settings by hand, ship this file:

```text
Data\SFSE\Plugins\OSFUI\settings\<mod-id>.json
```

Keep the id and setting keys stable after release because they identify saved values.

## Minimal example

```json
{
  "$schema": "https://raw.githubusercontent.com/ozooma10/osf-ui/refs/heads/main/docs/schema/settings-schema.schema.json",
  "title": "My Mod",
  "version": 1,
  "groups": [
    {
      "id": "general",
      "label": "General",
      "settings": [
        {
          "key": "enabled",
          "label": "Enabled",
          "type": "bool",
          "default": true
        },
        {
          "key": "strength",
          "label": "Strength",
          "type": "int",
          "min": 0,
          "max": 100,
          "step": 5,
          "default": 50,
          "format": { "suffix": "%" }
        }
      ]
    }
  ]
}
```

The `$schema` line enables autocomplete and validation in editors that support JSON Schema.

## Controls

| `type` | Value | Common fields |
|---|---|---|
| `bool` | `true` or `false` | |
| `int` | whole number | `min`, `max`, `step` |
| `float` | decimal number | `min`, `max`, `step` |
| `enum` | one string from `options` | `options`, `optionLabels` |
| `flags` | array of strings from `options` | `options`, `optionLabels` |
| `string` | text | `maxLength`, `widget` |
| `key` | rebindable physical key | `allowUnbound`, `onPress` |

Useful fields on any setting include:

- `label` and `hint` for player-facing text.
- `requires`: `restart`, `reload`, or `newGame`.
- `visibleWhen` and `enabledWhen` for display conditions.

Use `"type": "string", "widget": "color"` for a color picker. There is no `color` type.

The [settings JSON Schema](schema/settings-schema.schema.json) is the complete field reference, including pages, presets, notes, images, actions, localization identities, and scoped hotkeys.

## Read values in Papyrus

Use the getter matching the setting type:

```papyrus
Bool enabled = OSFUI_Settings.GetBool("yourname.mymod", "enabled", true)
Int strength = OSFUI_Settings.GetInt("yourname.mymod", "strength", 50)
String mode = OSFUI_Settings.GetString("yourname.mymod", "mode", "normal")
```

The last argument is returned when OSF UI, the mod, or the setting is unavailable. Use `OSFUI_Settings.ListenForChanges` when your script must react immediately; registrations are session-scoped and must be restored after loading a game. See the [settings Papyrus API](../data/Scripts/Source/OSFUI_Settings.psc) for the exact signatures.

Native plugins use [`OSFUI_API.h`](../sdk/OSFUI_API.h). Web views receive the registry through `osfui/settings` state and later commits through `settings.changed`; their types are in [`osfui.d.ts`](../sdk/osfui.d.ts).

## Add a Papyrus hotkey

A `key` setting can call a GLOBAL Papyrus function without a quest registration:

```json
{
  "key": "notifyKey",
  "label": "Show notification",
  "type": "key",
  "default": "F8",
  "allowUnbound": true,
  "onPress": {
    "script": "MyMod_Hotkeys",
    "function": "OnHotkey"
  }
}
```

```papyrus
ScriptName MyMod_Hotkeys Hidden

Function OnHotkey(string asModId, string asKey) Global
    Debug.Notification("Hotkey pressed")
EndFunction
```

The script name must match exactly, and the function must be GLOBAL with those two string parameters. Hotkeys dispatch during gameplay, not while a menu or the console is open.

## Check it in game

Enable the mod, load a save, press F10, and open its card. If the card is missing, check System Health and `OSF UI.log` for the schema error.

Ship the schema file, but do not ship `settings\values\`; OSF UI creates that folder for each player's saved values.
