# Migrating to OSF UI 2.0

OSF UI 2.0 is a WebView host, JavaScript bridge, compositor, and web-input add-on. Settings, hotkeys, and diagnostics moved to [OSF Settings](https://github.com/ozooma10/osf-settings-slim); install it alongside. A built-in [1.6 compatibility adapter](docs/compatibility-v1.md) supports the documented legacy baseline through OSF Settings. New and updated mods should use the current APIs below.

This guide applies when porting a mod to the new API. Existing mods within the
documented 1.6 compatibility baseline keep their compiled DLLs, scripts, manifests,
view paths, and saved values; they do not need these migration steps. Use the
[companion Settings release](sdk/vendor/README.md) in either case.

## Removed

- Modular exports `OSFUI_RequestSettings`, `OSFUI_RequestDiagnostics`, and `OSFUI_RequestViews`; `OSFUI_RequestBridge` is retained for the 1.6 adapter
- Headers `OSFUI_API.h`, `OSFUI_Settings.h`, `OSFUI_Diagnostics.h`, and `OSFUI_Views.h`; the `OSFUI::API::Views` namespace
- Scripts `OSFUI_Settings.psc` and `OSFUI_View.psc`
- The HTML Settings and Keybindings views, F10, Pause/Main Menu injection, deep links, and the default Settings view ID
- Manifest `hub`, `targetVersion`, catalog, and view-policy fields
- Automatic Settings data in the JavaScript bridge
- Native view-open preflight callbacks. Prepare mod state before requesting a menu,
  or initialize through browser requests after opening.
- Native readiness callbacks in the modern API. Register handlers and publish
  retained state before opening a view, or request fresh data from the page when
  it initializes. The frozen legacy `SetReadyCallback` remains available through
  `OSFUI_RequestBridge`.

## Replace with

| 1.x | 2.0 |
| --- | --- |
| `OSFUI_RequestViews`, `OSFUI::API::Views` | `OSFUI_RequestAPI`, `OSFUI::API::IUI` wrapped by `OSFUI::API::Client` ([OSFUI.h](sdk/OSFUI.h), API 2.0) |
| `OSFUI_View.psc` | [OSFUI.psc](data/Scripts/Source/OSFUI.psc); recompile scripts |
| Request replies | JSON only: `request.Reply(json)` |
| Settings, diagnostics, hotkeys | OSF Settings SDK: `Issue` with `Report` / `Clear`; `AcquireHotkeyBlock` / `ReleaseHotkeyBlock` |
| Opening a view from a hotkey | Register a callback hotkey with OSF Settings and call `Client::RequestMenu` from it ([example](examples/settings-view/README.md)) |

Always pass explicit qualified view IDs. Settings Papyrus APIs, actions, and localization are not part of this release.

Rebuild consumers of earlier **2.0 development versions** of `OSFUI.h` against the
release SDK. Their `IUI` layouts changed during development, including removal of
the preflight slots, while the API version remained 1.0. The release SDK requests
API 2.0 (`0x00020000`); the release DLL rejects API 1.x. Install the release DLL
alongside rebuilt consumers: acquisition fails when either side still uses API 1.0.
This rebuild requirement does not apply to supported 1.6 binaries using
`OSFUI_RequestBridge`. OSF UI 2.0 establishes the new API 2.0 contract through the
separate `OSFUI_RequestAPI` export; see [API versioning](docs/api-contract.md).

Browser code inspecting the ready envelope must use the integer
`payload.protocolVersion: 2`. The development `bridgeVersion: "2.0"` field is
removed; `payload.version` continues to identify the product release.

## Paths

Move views from `Data/SFSE/Plugins/OSFUI/views/<mod>/<view>/` to `Data/SFSE/Plugins/OSF/UI/views/<mod>/<view>/`. OSF UI installs only its own schema, `Data/SFSE/Plugins/OSF/Settings/schemas/osfui.json`; other schemas ship with their mod.

## Manifests

- Add `"manifestVersion": 1`; remove `hub` and `targetVersion`.
- Only HUD views with `openOnStart` autostart; `debugOnly` ones also need developer mode.
- `osfui/settings` and `osfui/keybinds` are rejected.

## Saved values

Legacy providers retain their original flat value files through the compatibility adapter. When updating a mod, re-author schemas for OSF Settings schema v1 and migrate values explicitly; modern values use `{"formatVersion":1,"values":{...}}` under OSF Settings' own data tree.

## State

Pages receive only what their mod publishes with `SetState` or `OSFUI.SetState`. Read OSF Settings in the mod and forward the values the page needs.
