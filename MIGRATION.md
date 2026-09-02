# Migrating to OSF UI 2.0

OSF UI 2.0 is an intentional compatibility break. It is now only a WebView
host, JavaScript bridge, compositor, and web-input add-on. Install OSF Settings
1.x alongside it.

## Removed from OSF UI

- `OSFUI_RequestBridge`, `OSFUI_RequestSettings`, and
  `OSFUI_RequestDiagnostics` exports
- `OSFUI_API.h`, `OSFUI_Settings.h`, and `OSFUI_Diagnostics.h`
- `OSFUI_Settings.psc`
- the HTML Settings and Keybindings views
- F10, Pause/Main Menu injection, deep links, and the default Settings view ID
- manifest `hub`, `targetVersion`, catalog, and view-policy fields
- automatic Settings data in the JavaScript bridge
- every legacy 1.x bridge/schema alias

Use `OSFUI_RequestViews`, `OSFUI_Views.h`, `OSFUI.psc`, and `OSFUI_View.psc`
with explicit qualified view IDs. Settings, actions, diagnostics, hotkeys,
localization, and suppression leases are provided by the
[OSF Settings SDK](https://github.com/ozooma10/osf-settings).

## Paths

Move views from:

`Data/SFSE/Plugins/OSFUI/views/<mod>/<view>/`

to:

`Data/SFSE/Plugins/OSF/UI/views/<mod>/<view>/`

OSF UI installs its own Settings schema at
`Data/SFSE/Plugins/OSF/Settings/schemas/osfui.json`. Other mod schemas belong
to the OSF Settings package or the owning mod.

View manifests now require `"manifestVersion": 1`. Remove `hub` and
`targetVersion`. Only HUD views satisfying
`kind == "hud" && openOnStart && (!debugOnly || developerMode)` autostart.
The stale IDs `osfui/settings` and `osfui/keybinds` are rejected.

## Saved values

There is no saved-value migration. OSF Settings does not read the old OSFUI
data tree. Re-author schemas for OSF Settings schema v1; new values are stored
as `{"formatVersion":1,"values":{...}}` beneath its independent data subtree.

## Explicit forwarding

Web pages receive only state their owning mod publishes with `SetViewState` or
`OSFUI_View.SetState`. Read settings through OSF Settings and forward the
minimal values needed by the page. This is a deliberate privacy and coupling
boundary.
