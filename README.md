# OSF UI

A web view framework for Starfield.

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

</details>

## Runtime

Requires SFSE, Address Library, OSF Settings, and the Edge WebView2 Evergreen Runtime. OSF UI starts the WebView2 helper only when a view is requested. It ships no views, menus, or hotkeys of its own.


- HUD autostart waits for page load. Loading and main-menu transitions suspend requested HUDs; they resume afterward and after successful browser recovery. Explicit close requests remain closed.
- Views load bundled local files only. Browser networking and page-requested external windows are blocked.
- Pages receive only state their owning mod publishes with `SetViewState` / `OSFUI.SetState`.
- A menu view joins the OSF Settings **Launcher** tab with `"launcher": { "modId": "mymod", "modTitle": "My Mod" }` in its manifest. Omit it for private views; Debug-only views appear only in developer mode.

## Build and test

Use XMake, an MSVC compiler with C++23 support. Deploy and package builds also compile `OSFUI.psc`; set `PAPYRUS_COMPILER` and `PAPYRUS_IMPORTS` if the Creation Kit compiler is outside the defaults in `tools/build-papyrus.ps1`.

```powershell
git submodule update --init --recursive
pwsh tools/setup.ps1
xmake f -P . -m releasedbg
xmake build
```

## Release package

```powershell
pwsh tools/package.ps1
```

Writes `dist/OSF-UI-<version>.zip` with its checksum. The archive owns the OSF UI binaries, `views/shared`, scripts, and `osfui.json`; it never removes the shared `OSF` parent or the OSF Settings subtree.

## License

[GPL-3.0](LICENSE) with the additional permissions in [EXCEPTIONS](EXCEPTIONS).
