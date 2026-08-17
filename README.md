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

The generator offers menu and HUD view starters with Papyrus or native-plugin
mod backends, plus a Papyrus settings-only starter that creates no view.

Papyrus projects compile a recordless GLOBAL library into a loose PEX; JavaScript calls any of its GLOBAL functions with `osfui.papyrus.call(script, function, ...args)` without an ESM, quest, registration, or Spriggit. `doctor` checks the Creation Kit compiler before the first build.

The harness opens automatically, hot-reloads edits, supplies the shared kit and mock bridge, and exposes bridge traffic and lifecycle controls.

`npm run dev:game -- --deploy "path-to-MO2-mods"`
also builds the mod backend, syncs changes into the game, and enables developer
mode through a temporary author-mode marker, including automatic view reload
and F12 DevTools. `npm run package` makes the loadable release zip.

See [the view toolchain guide](docs/view-toolchain.md) for the complete workflow.

## Mod API

The web bridge API has four verbs, chosen by desired behavior:

| Verb | Direction | Reach for it when |
|---|---|---|
| `osfui.send(name, payload)` | view → mod backend / OSF UI runtime | a one-way notification needs no response |
| `osfui.request(name, payload)` | view → mod backend / OSF UI runtime | you need exactly one answer: a payload, a typed error, or a timeout |
| `osfui.on(event, fn)` | mod backend / OSF UI runtime → view | something happened once — never replayed |
| `osfui.state.on(key, fn)` | mod backend / OSF UI runtime → view | a value remains current until replaced — replayed to every fresh document instance |

A view should not have any lifecycle code. Use `state` for data that should be
synchronized with its mod backend and replayed to each fresh document instance.

The typed reference is [`sdk/osfui.d.ts`](sdk/osfui.d.ts).

## Documentation

- [Authoring settings](docs/authoring-settings.md) — **start here to add settings to your mod**: schemas, widgets, hotkeys, localization, and testing.
- [View toolchain](docs/view-toolchain.md) and [view authoring reference](docs/authoring-views.md) — scaffold, develop, package, and integrate a browser view.
- [Dynamic data](docs/authoring-dynamic-data.md) and [native plugin API](docs/native-plugin-api.md) — state, events, requests, and the SFSE C ABI.
- [Architecture](docs/architecture.md), [security model](docs/security-model.md), [logging](docs/logging.md), and [seam rendering design](docs/seam-draw-design.md) — OSF UI runtime implementation and invariants.
- [Terminology](docs/terminology.md) — canonical component, version, identity,
  lifecycle, bridge, and input vocabulary.
- [Mod API 2.0 design](docs/mod-api-2.0-design.md) and [migration record](docs/mod-api-2.0-migration.md) — rationale and compatibility history.
- [Packaging](docs/PACKAGING.md), [troubleshooting](docs/troubleshooting.md), [design-history index](docs/design-history.md), and [simplification notes](docs/simplification.md) — maintainer and support references.
- JSON Schemas: [view manifests](docs/schema/manifest.schema.json) and [settings schemas](docs/schema/settings-schema.schema.json).

## Install / paths

Final layout (game or mod folder):

```
Data/SFSE/Plugins/
  OSFUI.dll
  OSFUI/                 <- plugin data, resolved relative to the DLL
    config.json
    views/                     <- GENERATED from frontend/ (see "Building the frontend")
      shared/                     the shared UI kit — third-party views link it by exact path
        osfui.css  osfui.js
      osfui/                      <- a mod namespace: views live at views/<modId>/<viewName>/
        padnav.js                    gamepad nav, private to the built-in views
        settings/                    the Mod Settings view
          manifest.json
          index.html  style.css  main.js
        keybinds/                    the Keybindings view
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

`OSFUI/config.json` is the **developer/boot file** — framework enable, default
view selection, and persistent developer mode.
It ships with the mod and is overwritten on update; it holds no user-facing keys. 

The keys you might actually edit:

| field | default | meaning |
|---|---|---|
| `enabled` | `true` | master switch |
| `view` | `"osfui/settings"` | the default menu the toggle key opens — a qualified `<modId>/<viewName>` id derived from the `views/<modId>/<viewName>/` path (shipped config uses the Mod Settings view) |
| `devMode` | `false` | persistently enables developer mode: verbose logging, hot reload, and F12 DevTools |

With developer mode enabled (`devMode` or the temporary author-mode marker), saved changes to an instantiated view's files auto-reload it in place within about half a second.
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
npm ci # once per fresh clone
xmake build
```

Output lands in `build/windows/x64/<mode>/`. To deploy automatically, set one of (before configuring):

- `XSE_SF_MODS_PATH` - a mod manager `mods` folder → installs to `<mods>/OSF UI/SFSE/Plugins/...`
- `XSE_SF_GAME_PATH` - the game folder → installs to `Data/SFSE/Plugins/...`

The install includes the DLL, PDB, and the `OSFUI/` data folder (config + views).

## Building the frontend

The built-in views are **not** hand-edited. Their source is a Vite + TypeScript + Preact project in [`frontend/`](frontend/README.md), which generates the ignored `build/frontend/views/` artifact:

```bat
npm ci                          # once
npm --prefix frontend run build # regenerate build/frontend/views/
```

> **`build/frontend/views/` is disposable build output.** Edit `frontend/src/`; the generated tree is replaced on every build and is never committed.

`xmake build` and `xmake install` generate this artifact before deploying or staging it; `tools/package.ps1` installs locked frontend dependencies first.


## WebView2 rendering

`WebView2HostWebRenderer` is the game-side web renderer. It communicates with
the out-of-process browser host shown above:

```bat
xmake build
```

The build uses the static WebView2 loader from the SDK package; users still
need the Evergreen WebView2 Runtime installed. The install step ships
`osfui_webview2_host.exe` in the `OSFUI/bin/` folder shown above.

## Credits & acknowledgments

See [CREDITS.md](CREDITS.md)

## License

GPL-3.0 See `LICENSE` and `EXCEPTIONS`.
