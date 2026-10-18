# OSF UI

A web view framework for Starfield.

OSF UI **2.1.0** is prepared as separate WebView2 and CEF editions with one shared
DLL and API. See the [2.1 release record](docs/release-2.1.md) for candidate identity,
completed validation and outstanding release checks.

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
- [Browser editions and architecture](docs/browser-backends.md)

</details>

## Runtime

Both editions require SFSE, Address Library, and the companion OSF Settings 1.0.0 release or a compatible newer release.

| Edition | Recommendation | Browser requirement |
| --- | --- | --- |
| **WebView2 — recommended default** | Choose this for most installations: smaller download and lower measured browser CPU and memory overhead. | Microsoft Edge WebView2 Evergreen Runtime |
| **CEF — bundled-runtime alternative** | Choose this when you want the browser runtime included with OSF UI. It has a larger download and higher measured CPU and memory overhead. | Bundled Chromium; no WebView2 runtime required |

Install **one** edition. The package automatically selects its renderer; no settings change is needed. Its `SFSE/Plugins/OSF/UI/backend.json` selection file is optional at runtime: if absent, OSF UI uses WebView2. Both contain the same game DLL and public APIs, so mods use the same integration with either edition. See [browser editions and measured tradeoffs](docs/browser-backends.md) for the evidence and switching instructions. Both editions use the [packaged in-game acceptance suite](tests/game/README.md).

During release testing, use the [matching Settings source baseline](sdk/vendor/README.md); older development builds also labeled 1.0.0 may have incompatible interfaces. OSF UI starts the selected browser helper only when a view is requested. It ships no views, menus, or hotkeys of its own.


- HUD autostart waits for page load. Loading and main-menu transitions suspend requested HUDs; they resume afterward and after successful browser recovery. Explicit close requests remain closed.
- View files are prepared in the background after game data loads. Each session reuses that preparation for view opens and browser recovery; unchanged disk caches are reused across sessions. An earlier view request starts or waits for the same preparation job.
- Views load bundled local files only. Browser networking and page-requested external windows are blocked.
- Pages receive only state their owning mod publishes with `SetState` / `OSFUI.SetState`.
- A menu view joins the OSF Settings **Launcher** tab with `"launcher": true` in its manifest. The launcher mod ID defaults to the mod folder name and its display name defaults to the view's `title`. Use an object such as `"launcher": { "modId": "mymod", "modTitle": "My Mod" }` only for overrides; both fields are optional. Launcher IDs allow lowercase letters, digits, `.`, `_`, and `-`; other folder names need an explicit valid override. Omit `launcher` or set it to `false` for private and HUD views; Debug-only views appear only in developer mode.

## Build and test

Use XMake, an MSVC compiler with C++23 support. Deploy and package builds also compile `OSFUI.psc`; set `PAPYRUS_COMPILER` and `PAPYRUS_IMPORTS` if the Creation Kit compiler is outside the defaults in `tools/build-papyrus.ps1`.

```powershell
git submodule update --init --recursive
pwsh tools/setup.ps1
xmake f -P . -m releasedbg --browser_backend=webview2
xmake build
```

Native unit tests need no game or SFSE. `tests/native/run.sh` runs the full suite with clang++ or g++, or falls back to MSVC through `vcvars64`; its exit code is the failure count. A subset, including the V1 compatibility tests, is also registered with XMake.

```powershell
bash tests/native/run.sh
xmake test
node --test tests/native/compat_web_tests.cjs
```

The lifecycle suite executes the real Runtime, bridge, presentation, and input-capture code with desktop doubles for the game, renderer, and peer services. Its ten scenarios cover startup ordering, backend selection, failed dependencies and input integration, state replay and session reset, menu/HUD transitions, suppression leases, and browser recovery. Each scenario runs in a separate process to preserve the production singleton lifetimes.

```powershell
xmake test -j4 'osfui-runtime-lifecycle-tests/*'
```

The doubles supply load, engine-focus, and frame-readiness events. SFSE hook delivery, actual engine counters, WebView2 capture, and GPU completion still require in-game testing; these tests do not read or match source text.

## Release package

```powershell
pwsh tools/package.ps1
```

Writes separate `dist/OSF-UI-v<version>-<tag>-webview2.zip` (**recommended default**) and `-cef.zip` (**bundled-runtime alternative**) archives (`-Tag` defaults to `rc`), plus SHA-256 and manifest sidecars. Use `-Backend webview2` or `-Backend cef` for one edition. The common DLL, scripts, and web assets are byte-identical in both archives; each supplies its own `backend.json`. Packaging verifies the archives locally and never publishes. The archive owns the OSF UI binaries, `views/shared`, scripts, and `osfui.json`; it never removes the shared `OSF` parent or the OSF Settings subtree.

The [Nexus release fields](packaging/nexus/release-2.1-fields.txt) include separate file titles and descriptions for the two editions; the [main description](packaging/nexus/description-2.1.bbcode.txt) explains their requirements and installation choices. Use `-Tag ''` for final filenames. Building archives does not publish them.

## License

[GPL-3.0](LICENSE) with the additional permissions in [EXCEPTIONS](EXCEPTIONS).
