# OSF UI

[![CI](https://github.com/ozooma10/osf-ui/actions/workflows/ci.yml/badge.svg)](https://github.com/ozooma10/osf-ui/actions/workflows/ci.yml)

**OSF UI** is an SFSE/CommonLibSF plugin that hosts HTML/CSS/JS UI views over Starfield. 
It is heavily inspired by **[Prisma UI](https://www.prismaui.dev/)
by StarkMP**

## Requirements

- [XMake](https://xmake.io) 3.0.0+
- C++23 compiler (MSVC / Clang-CL)

## Build

```bat
xmake build
```

Output lands in `build/windows/x64/<mode>/`. To deploy automatically, set one
of (before configuring):

- `XSE_SF_MODS_PATH` - a mod manager `mods` folder → installs to `<mods>/OSF UI/SFSE/Plugins/...`
- `XSE_SF_GAME_PATH` - the game folder → installs to `Data/SFSE/Plugins/...`

The install includes the DLL, PDB, and the `OSFUI/` data folder (config + views).

## Documentation

- [docs/troubleshooting.md](docs/troubleshooting.md) - **players start here**: requirements, install, troubleshooting, uninstall, and known limitations
- [docs/authoring-views.md](docs/authoring-views.md) - **start here to build a view**: package layout, manifest fields, the bridge protocol, and the settings schema format
- [docs/architecture.md](docs/architecture.md) - layers and data flow
- [docs/security-model.md](docs/security-model.md) - JS-is-untrusted rules
- [docs/reverse-engineering-notes.md](docs/reverse-engineering-notes.md) - what is unknown and must not be guessed

## Install / paths

Final layout (game or mod folder):

```
Data/SFSE/Plugins/
  OSFUI.dll
  OSFUI/                 <- plugin data, resolved relative to the DLL
    config.json
    views/
      settings/                <- built-in views
        manifest.json
        index.html  style.css  main.js
    ultralight/                <- only present in with_ultralight builds
      bin/                        (delay-loaded at runtime, preloaded by the plugin)
        Ultralight.dll  UltralightCore.dll  WebCore.dll  AppCore.dll
      resources/
        icudt67l.dat            (ICU data; cacert.pem deliberately not shipped)
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
| `views` | `[]` | optional multi-view set: every id is loaded and composited (layer order = each view's manifest `zorder`), and `view` must be one of them (the interactive one). Empty ⇒ only `view` loads. Missing ids are skipped with a log line. Shipped config uses `["osfui/settings", "osfui/keybinds"]` |
| `devMode` | `false` | verbose per-call logging + first-frame PNG dump - turn on when developing views or attaching logs to a bug report |
| `devReloadKey` | `"F11"` | with `devMode` on, reloads the top open menu's URL in place (the fast view-iteration loop); ignored otherwise |

The remaining keys (`renderer`, `compositor`, `inputSource`, `captureInput`,
`hardwareCursor`, `focusMenu`, `engineInput`, `pauseMenuEntryLabel`/`View`,
`configVersion`) select backends and serve as diagnostic escape hatches - the
shipped values are the only supported configuration.

## Ultralight backend

The default build has **zero** Ultralight footprint and must stay that way.
To compile the real renderer:

```bat
set ULTRALIGHT_SDK_DIR=C:\path\to\ultralight-sdk
xmake f --with_ultralight=true
xmake build
```

The build fails with a clear message if `ULTRALIGHT_SDK_DIR` is missing, and the install step ships the SDK's runtime DLLs + ICU data into the `OSFUI/ultralight/` folder shown above. 

## Credits & acknowledgments

**OSF UI** exists because of **[Prisma UI](https://www.prismaui.dev/)**,
the Skyrim Special Edition web-UI framework by **StarkMP**. Prisma UI pioneered the approach this project is built
around - rendering modern HTML/CSS/JS interfaces in game using the
Ultralight engine - and the entire idea for a Starfield equivalent came from it.

- **[Prisma UI](https://www.nexusmods.com/skyrimspecialedition/mods/148718)** —
  StarkMP & contributors - original concept and inspiration.
- **[Ultralight](https://ultralig.ht/)** - Ultralight, Inc. - the lightweight,
  WebKit-based renderer behind every view (used under the Ultralight Free
  License; notices ship in `OSFUI/ultralight/license/`).
- **[commonlibsf-template](https://github.com/libxse/commonlibsf-template)** /
  **CommonLibSF** & **[SFSE](https://sfse.silverlock.org/)** -= the plugin
  foundation this is built on.

See [CREDITS.md](CREDITS.md)

## License

GPL-3.0 See `LICENSE` and `EXCEPTIONS`.
