# OSF UI

A WebView2 add-on for Starfield mods: view discovery, the JavaScript/native/Papyrus bridge, D3D12 compositing, focus, and web input. Settings, hotkeys, and issue reporting belong to [OSF Settings](https://github.com/ozooma10/osf-settings-slim).

For mod authors, read the API for your side of the bridge.

| Reference | Use it to |
| --- | --- |
| [osfui.d.ts](sdk/osfui.d.ts) | Call `send`, `request`, `on`, and `state` from a page |
| [OSFUI.h](sdk/OSFUI.h) | Open views, handle messages, and publish state from C++ (`OSFUI::API::Client` via `OSFUI_RequestAPI`) |
| [OSFUI.psc](data/Scripts/Source/OSFUI.psc) | The same from Papyrus |
| [manifest.schema.json](docs/schema/manifest.schema.json) | Describe a view |
| [Settings example](examples/settings-view/README.md) | Open a view from an OSF Settings hotkey and forward a setting |

<details>
<summary>additional topics</summary>

- [Migrating from 1.x](MIGRATION.md)
- [Forwarded input](docs/cdp-input-prototype.md): how keyboard and text reach WebView2
- [Profiling](tools/profiling/README.md)

</details>

## Runtime

Requires Starfield 1.16.244, SFSE, Address Library, OSF Settings (settings and diagnostics ABI 1.0), and the Edge WebView2 Evergreen Runtime. OSF UI stays inert until OSF Settings is ready at `kPostPostLoad` and starts the WebView2 helper only when a view is requested. It ships no views, menus, or hotkeys of its own.

```text
Data/SFSE/Plugins/OSFUI.dll
Data/SFSE/Plugins/OSF/UI/bin/osfui_webview2_host.exe
Data/SFSE/Plugins/OSF/UI/views/<mod-id>/<view-name>/
Data/SFSE/Plugins/OSF/Settings/schemas/osfui.json
```

- Pages receive only state their owning mod publishes with `SetViewState` / `OSFUI.SetState`. Read OSF Settings in your mod and forward what the page needs.
- A menu view joins the OSF Settings **Launcher** tab with `"launcher": { "modId": "mymod", "modTitle": "My Mod" }` in its manifest. Omit it for private views; HUD views cannot opt in. Debug-only views appear only in developer mode.
- `developerMode` and `highRefreshCapture` are read once at startup; changes need a restart.
- A WebView failure releases input and shows as a Mod Issue in OSF Settings; details go to the native log.

## Build and test

Use XMake, an MSVC compiler with C++23 support. Deploy and package builds also compile `OSFUI.psc`; set `PAPYRUS_COMPILER` and `PAPYRUS_IMPORTS` if the Creation Kit compiler is outside the defaults in `tools/build-papyrus.ps1`.

```powershell
git submodule update --init --recursive
pwsh tools/setup.ps1
xmake f -P . -m releasedbg
xmake build -P . -y "OSF UI"
```

```sh
bash tests/native/run.sh   # Bash 4+ and a C++23 compiler
```

## Release package

```powershell
pwsh tools/package.ps1
```

Writes `dist/OSF-UI-<version>.zip` with its checksum. The archive owns `OSF/UI` and `osfui.json` only; it never removes the shared `OSF` parent or the OSF Settings subtree.

## License

[GPL-3.0](LICENSE) with the additional permissions in [EXCEPTIONS](EXCEPTIONS). See [CREDITS.md](CREDITS.md).
