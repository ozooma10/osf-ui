# OSF UI

[![CI](https://github.com/ozooma10/osf-ui/actions/workflows/ci.yml/badge.svg)](https://github.com/ozooma10/osf-ui/actions/workflows/ci.yml)

**OSF UI** is an SFSE plugin that hosts Web views over Starfield via Microsoft WebView2.

It also provides a Settings system for mods to interface with.

## Developing a third-party view

Create a complete project and open its browser harness:

```bat
npm create osfui@latest my-view
cd my-view
npm run doctor
npm run dev
```

The generator offers menu or HUD surfaces with Papyrus or native-plugin backends.

Papyrus projects include reproducible Spriggit quest records and compile their ESM and PEX files automatically; `doctor` checks Spriggit and Creation Kit before the first native build.

The harness opens automatically, hot-reloads edits, supplies the shared kit and mock bridge, and exposes bridge traffic and lifecycle controls.

`npm run dev:game -- --deploy "path-to-MO2-mods"`
also builds the backend, syncs changes into the game, and enables temporary author mode, including automatic view reload and F12 DevTools. `npm run package` makes the loadable release zip.

See [the view toolchain guide](docs/view-toolchain.md) for the complete workflow.

## Mod API

The whole web surface is four verbs, chosen on desired behavior

| Verb | Direction | Reach for it when |
|---|---|---|
| `osfui.send(name, payload)` | view → game | send to game and no response needed. |
| `osfui.request(name, payload)` | view → game | you need exactly one answer: a payload, a typed error, or a timeout |
| `osfui.on(event, fn)` | game → view | something happened once - never replayed (Only triggered when occurs ingame) |
| `osfui.state.on(key, fn)` | game → view | a value that stays true until it changes - always replayed (ex. view reload) |

A view should not have any lifecycle code. Use `state` for any data that should be synchronized with the backend.

the typed reference is [`sdk/osfui.d.ts`](sdk/osfui.d.ts).

## Documentation

- [docs/authoring-settings.md](docs/authoring-settings.md) - **start here to add settings to your mod**: one JSON file, no code - quickstart, widgets, hotkeys, presets, localization, testing
- [docs/view-toolchain.md](docs/view-toolchain.md) - **start here to build a view**: scaffold, browser HMR, in-game sync, checks, and packaging
- [docs/authoring-views.md](docs/authoring-views.md) - view manifest and bridge protocol reference
- [docs/authoring-dynamic-data.md](docs/authoring-dynamic-data.md) - state vs. events: feeding a view live game data from Papyrus or a plugin, and surviving reload
- [docs/native-plugin-api.md](docs/native-plugin-api.md) - the C ABI for SFSE plugins (`sdk/OSFUI_API.h`)
- [docs/troubleshooting.md](docs/troubleshooting.md) - requirements, install, troubleshooting, uninstall, and known limitations

## Install / paths

Final layout (game or mod folder):

```
Data/SFSE/Plugins/
  OSFUI.dll
  OSFUI/                 <- plugin data, resolved relative to the DLL
    config.json
    vanillakeys.json
    views/                     <- GENERATED from frontend/ (see "Building the frontend")
      shared/                     the shared UI kit — third-party views link it by exact path
        osfui.css  osfui.js
      osfui/                      <- a mod namespace: views live at views/<modId>/<viewName>/
        padnav.js                    gamepad nav, private to the built-in views
        settings/                    the Mods surface
          manifest.json
          index.html  style.css  main.js
        keybinds/                    the input map
          manifest.json
          index.html  style.css  main.js
    settings/                  <- settings schemas (one JSON per mod) + values/
    bin/
      osfui_webview2_host.exe   <- out-of-process browser host
```

Logs go to the standard SFSE log folder (`Documents/My Games/Starfield/SFSE/Logs/OSF UI.log`).

## Config

**User-facing settings live in the in-game menu** (F10 → OSF UI): the open/close key. 
They persist under `Documents\My Games\Starfield\OSFUI\settings\osfui.json` and survive updates.

`OSFUI/config.json` is the **developer/boot file** - backends, input source, diagnostic escape hatches. 
It ships with the mod and is overwritten on update; it holds no user-facing keys. 

The keys you might actually edit:

| field | default | meaning |
|---|---|---|
| `enabled` | `true` | master switch |
| `view` | `"osfui/settings"` | the default menu the toggle key opens - a qualified `<modId>/<viewName>` id from `views/<modId>/<viewName>/manifest.json` (shipped config uses `osfui/settings`, the Mods surface) |
| `devMode` | `false` | verbose per-call logging + first-frame PNG dump - turn on when developing views or attaching logs to a bug report |

With `devMode` enabled, saved changes to a loaded view's files auto-reload it in place within about half a second.
Press **F12** while a menu is open to inspect that view in WebView2's native Edge DevTools.


## Requirements

- [XMake](https://xmake.io) 3.0.0+
- Microsoft Edge WebView2 Runtime (Evergreen)
- Microsoft.Web.WebView2 SDK package unpacked to `external/webview2`, or `WEBVIEW2_SDK_DIR` set to its package root
- C++23 compiler (MSVC / Clang-CL)

## First-time setup

Run the setup script once. It fetches the Microsoft.Web.WebView2 SDK and unpacks them into `external/webview2`:

```bat
pwsh tools/setup.ps1
```

## Build

```bat
npm --prefix frontend ci # once per fresh clone
xmake build
```

Output lands in `build/windows/x64/<mode>/`. To deploy automatically, set one of (before configuring):

- `XSE_SF_MODS_PATH` - a mod manager `mods` folder → installs to `<mods>/OSF UI/SFSE/Plugins/...`
- `XSE_SF_GAME_PATH` - the game folder → installs to `Data/SFSE/Plugins/...`

The install includes the DLL, PDB, and the `OSFUI/` data folder (config + views).

## Building the frontend

The built-in views are **not** hand-edited. Their source is a Vite + TypeScript + Preact project in [`frontend/`](frontend/README.md), which generates the ignored `build/frontend/views/` artifact:

```bat
npm --prefix frontend ci        # once
npm --prefix frontend run build # regenerate build/frontend/views/
```

> **`build/frontend/views/` is disposable build output.** Edit `frontend/src/`; the generated tree is replaced on every build and is never committed.

`xmake build` and `xmake install` generate this artifact before deploying or staging it; `tools/package.ps1` installs locked frontend dependencies first.


## WebView2 backend

WebView2 is the production renderer and is enabled by default:

```bat
xmake f --with_webview2=true
xmake build
```

The build uses the static WebView2 loader from the SDK package; users still
need the Evergreen WebView2 Runtime installed. The install step ships
`osfui_webview2_host.exe` in the `OSFUI/bin/` folder shown above.

## Credits & acknowledgments

See [CREDITS.md](CREDITS.md)

## License

GPL-3.0 See `LICENSE` and `EXCEPTIONS`.
