# OSF UI

[![CI](https://github.com/ozooma10/osf-ui/actions/workflows/ci.yml/badge.svg)](https://github.com/ozooma10/osf-ui/actions/workflows/ci.yml)

**OSF UI** is an SFSE/CommonLibSF plugin that hosts HTML/CSS/JS UI views over Starfield. 
It is heavily inspired by **[Prisma UI](https://www.prismaui.dev/)
by StarkMP**

## Requirements

- [XMake](https://xmake.io) 3.0.0+
- Microsoft Edge WebView2 Runtime (Evergreen)
- Microsoft.Web.WebView2 SDK package unpacked to `external/webview2`, or `WEBVIEW2_SDK_DIR` set to its package root
- C++23 compiler (MSVC / Clang-CL)

## First-time setup

On a fresh clone, run the setup script once. It fetches the build-time
dependencies that are **not** checked into the repo — currently the
Microsoft.Web.WebView2 SDK — and unpacks them into `external/webview2`:

```bat
pwsh tools/setup.ps1
```

Without this, `xmake build` fails with `OSFUI WebView2 host: unpack
Microsoft.Web.WebView2 into external/webview2 ...` because `external/` is
gitignored. The script is idempotent; pass `-Force` to re-fetch. It does **not**
install xmake, the Edge WebView2 Evergreen runtime, or Node — those are listed
under Requirements above (and Node only for [building the frontend](#building-the-frontend)).

## Build

```bat
xmake build
```

Output lands in `build/windows/x64/<mode>/`. To deploy automatically, set one
of (before configuring):

- `XSE_SF_MODS_PATH` - a mod manager `mods` folder → installs to `<mods>/OSF UI/SFSE/Plugins/...`
- `XSE_SF_GAME_PATH` - the game folder → installs to `Data/SFSE/Plugins/...`

The install includes the DLL, PDB, and the `OSFUI/` data folder (config + views).

## Building the frontend

The built-in views are **not** hand-edited. Their source is a Vite + TypeScript +
Preact project in [`frontend/`](frontend/README.md), which generates
`data/OSFUI/views/`:

```bat
npm --prefix frontend ci        # once
npm --prefix frontend run build # regenerate data/OSFUI/views/
```

> **`data/OSFUI/views/` is generated build output.** Edit `frontend/src/`, never
> the files under `data/`. Hand edits there are destroyed by the next build and
> CI fails the moment the two disagree.

The generated tree is **committed** on purpose: `xmake install`, the MO2
after-build redeploy and `tools/package.ps1` all read it directly, and none of
them can run Node. Node is therefore a *frontend build* dependency only — never
a runtime one, and not needed for `xmake build`, the native tests, or
`xmake install`. `npm --prefix frontend run dev` serves the views in a browser
with a mock bridge; `npm --prefix frontend run verify` is the pre-push gate.

See [frontend/README.md](frontend/README.md) for the full command set and
[frontend/COMPATIBILITY.md](frontend/COMPATIBILITY.md) for the artifacts that
are deliberately shipped verbatim.

## Documentation

- [docs/authoring-settings.md](docs/authoring-settings.md) - **start here to add settings to your mod**: one JSON file, no code — quickstart, widgets, hotkeys, presets, localization, testing
- [docs/authoring-views.md](docs/authoring-views.md) - **start here to build a view**: package layout, manifest fields, the bridge protocol, and the settings schema format
- [frontend/README.md](frontend/README.md) - **start here to change a built-in view**: the Vite/TS/Preact source that generates `data/OSFUI/views/`
- [docs/architecture.md](docs/architecture.md) - layers and data flow
- [docs/security-model.md](docs/security-model.md)
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

`OSFUI/config.json` is the **developer/boot file** - backends, input source, diagnostic escape hatches, the view set. 
It ships with the mod and is overwritten on update; it holds no user-facing keys. 

The keys you might actually edit:

| field | default | meaning |
|---|---|---|
| `enabled` | `true` | master switch |
| `view` | `"osfui/settings"` | the active (input) view the toggle key opens - a qualified `<modId>/<viewName>` id from `views/<modId>/<viewName>/manifest.json` (shipped config uses `osfui/settings`, the Mods surface) |
| `views` | `[]` | optional multi-view set: every id is loaded and composited (layer order is set by the menu/HUD framework — HUDs beneath open menus), and `view` must be one of them (the interactive one). Empty ⇒ only `view` loads. Missing ids are skipped with a log line. Shipped config uses `["osfui/settings", "osfui/keybinds"]` |
| `devMode` | build-mode | verbose per-call logging. **Omitted from the shipped config on purpose**: it defaults **on** in a local `debug` build and **off** in a `releasedbg`/release build, so a dev loop is chatty and a shipped release is quiet with nothing to remember to flip. Add `"devMode": true` here to force it on regardless (e.g. developing views or attaching logs to a bug report) |
| `devReloadKey` | `"F11"` | with `devMode` on, reloads the top open menu's URL in place (the fast view-iteration loop); ignored otherwise |

The remaining keys (`renderer`, `compositor`, `inputSource`, `captureInput`,
`hardwareCursor`, `focusMenu`, `engineInput`, `pauseMenuEntryLabel`/`View`,
`configVersion`) select backends and serve as diagnostic escape hatches - the
shipped values are the only supported configuration.

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
