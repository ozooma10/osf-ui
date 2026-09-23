# Migrating to OSF UI 2.0

OSF UI 2.0 is only a WebView host, JavaScript bridge, compositor, and web-input add-on. Settings, hotkeys, and diagnostics moved to [OSF Settings](https://github.com/ozooma10/osf-settings-slim); install it alongside (settings and diagnostics ABI 1.0). There are no compatibility aliases or adapters.

## Removed

- Exports `OSFUI_RequestBridge`, `OSFUI_RequestSettings`, `OSFUI_RequestDiagnostics`, and `OSFUI_RequestViews`
- Headers `OSFUI_API.h`, `OSFUI_Settings.h`, `OSFUI_Diagnostics.h`, and `OSFUI_Views.h`; the `OSFUI::API::Views` namespace
- Scripts `OSFUI_Settings.psc` and `OSFUI_View.psc`
- The HTML Settings and Keybindings views, F10, Pause/Main Menu injection, deep links, and the default Settings view ID
- Manifest `hub`, `targetVersion`, catalog, and view-policy fields
- Automatic Settings data in the JavaScript bridge

## Replace with

| 1.x | 2.0 |
| --- | --- |
| `OSFUI_RequestViews`, `OSFUI::API::Views` | `OSFUI_RequestAPI`, `OSFUI::API::IUI` wrapped by `OSFUI::API::Client` ([OSFUI.h](sdk/OSFUI.h), API 1.0) |
| `OSFUI_View.psc` | [OSFUI.psc](data/Scripts/Source/OSFUI.psc); recompile scripts |
| Request replies | JSON only: `request.Respond(json)` |
| Settings, diagnostics, hotkeys | OSF Settings SDK: `Issue` with `Report` / `Clear`; `AcquireHotkeyBlock` / `ReleaseHotkeyBlock` |
| Opening a view from a hotkey | Register a callback hotkey with OSF Settings and call `Client::RequestMenu` from it ([example](examples/settings-view/README.md)) |

Always pass explicit qualified view IDs. Settings Papyrus APIs, actions, and localization are not part of this release.

## Paths

Move views from `Data/SFSE/Plugins/OSFUI/views/<mod>/<view>/` to `Data/SFSE/Plugins/OSF/UI/views/<mod>/<view>/`. OSF UI installs only its own schema, `Data/SFSE/Plugins/OSF/Settings/schemas/osfui.json`; other schemas ship with their mod.

## Manifests

- Add `"manifestVersion": 1`; remove `hub` and `targetVersion`.
- Only HUD views with `openOnStart` autostart; `debugOnly` ones also need developer mode.
- `osfui/settings` and `osfui/keybinds` are rejected.

## Saved values

Not migrated. Re-author schemas for OSF Settings schema v1; values are stored as `{"formatVersion":1,"values":{...}}` under its own data tree.

## State

Pages receive only what their mod publishes with `SetViewState` or `OSFUI.SetState`. Read OSF Settings in the mod and forward the values the page needs.
