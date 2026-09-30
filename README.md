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
- [Built-in 1.6 compatibility](docs/compatibility-v1.md)
- [2.0 API contract and versioning](docs/api-contract.md)

</details>

## Runtime

Requires SFSE, Address Library, the companion OSF Settings 1.0.0 release, and the Edge WebView2 Evergreen Runtime. During release testing, use the [matching Settings source baseline](sdk/vendor/README.md); older development builds also labeled 1.0.0 may have incompatible interfaces. OSF UI starts the WebView2 helper only when a view is requested. It ships no views, menus, or hotkeys of its own.


- HUD autostart waits for page load. Loading and main-menu transitions suspend requested HUDs; they resume afterward and after successful browser recovery. Explicit close requests remain closed.
- Views load bundled local files only. Browser networking and page-requested external windows are blocked.
- Pages receive only state their owning mod publishes with `SetState` / `OSFUI.SetState`.
- A menu view joins the OSF Settings **Launcher** tab with `"launcher": { "modId": "mymod", "modTitle": "My Mod" }` in its manifest. Omit it for private views; Debug-only views appear only in developer mode.

## Build and test

Use XMake, an MSVC compiler with C++23 support. Deploy and package builds also compile `OSFUI.psc`; set `PAPYRUS_COMPILER` and `PAPYRUS_IMPORTS` if the Creation Kit compiler is outside the defaults in `tools/build-papyrus.ps1`.

```powershell
git submodule update --init --recursive
pwsh tools/setup.ps1
xmake f -P . -m releasedbg
xmake build
```

Native unit tests need no game or SFSE. `tests/native/run.sh` runs the full suite with clang++ or g++, or falls back to MSVC through `vcvars64`; its exit code is the failure count. A subset, including the V1 compatibility tests, is also registered with XMake.

```powershell
bash tests/native/run.sh
xmake test
node --test tests/native/compat_web_tests.cjs
```

## Release package

```powershell
pwsh tools/package.ps1
```

Writes `dist/OSF-UI-v<version>-<tag>.zip` (`-Tag` defaults to `rc`; pass `-Tag ""` for a final release) and prints its SHA-256. The archive owns the OSF UI binaries, `views/shared`, scripts, and `osfui.json`; it never removes the shared `OSF` parent or the OSF Settings subtree.

## License

[GPL-3.0](LICENSE) with the additional permissions in [EXCEPTIONS](EXCEPTIONS).
