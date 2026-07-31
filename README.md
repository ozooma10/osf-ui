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
under Requirements above. Node is required because xmake builds the built-in views.

## Build

```bat
npm --prefix frontend ci # once per fresh clone / lockfile update
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
the ignored `build/frontend/views/` artifact:

```bat
npm --prefix frontend ci        # once
npm --prefix frontend run build # regenerate build/frontend/views/
```

> **`build/frontend/views/` is disposable build output.** Edit `frontend/src/`;
> the generated tree is replaced on every build and is never committed.

`xmake build` and `xmake install` generate this artifact before deploying or
staging it; `tools/package.ps1` installs locked frontend dependencies first.
Node is therefore a developer/build dependency, never a player runtime
dependency. The native test suite remains Node-free. `npm --prefix frontend run
dev` serves the views in a browser with a mock bridge; `npm --prefix frontend
run verify` is the pre-push gate.

See [frontend/README.md](frontend/README.md) for the full command set and
[frontend/COMPATIBILITY.md](frontend/COMPATIBILITY.md) for the artifacts that
are deliberately shipped verbatim.

## Developing a third-party view

Create a complete project and open its browser harness:

```bat
npm create osfui@latest my-view
cd my-view
npm run doctor
npm run dev
```

The generator offers menu or HUD surfaces with Papyrus or native-plugin
backends. Papyrus projects include reproducible Spriggit quest records and
compile their ESM and PEX files automatically; `doctor` checks Spriggit and
Creation Kit before the first native build. The harness opens automatically,
hot-reloads edits, supplies the shared kit and mock bridge, and exposes bridge
traffic and lifecycle controls. `npm run dev:game -- --deploy "path-to-MO2-mods"`
also builds the backend, syncs changes into the game, and enables temporary
author mode, including automatic view reload and F12 DevTools. `npm run
package` makes the loadable release zip.

See [the view toolchain guide](docs/view-toolchain.md) for the complete workflow.

## Mod API 2.0

**2.0 is a hard break for views and native plugins.** Settings schemas are
unaffected — they are declarative data and execute nothing.

The whole web surface is four verbs, chosen by what you mean rather than by how
the message travels:

| Verb | Direction | Reach for it when |
|---|---|---|
| `osfui.send(name, payload)` | view → game | nothing has to come back |
| `osfui.request(name, payload)` | view → game | you need exactly one answer: a payload, a typed error, or a timeout |
| `osfui.on(event, fn)` | game → view | something happened once — never replayed |
| `osfui.state.on(key, fn)` | game → view | a value that stays true until it changes — always replayed |

The events/state split is the load-bearing one, and it is most of why 2.0
exists. Replaying an event on reload re-fires its effect; *not* replaying state
on reload is the blank HUD after F5 that 1.x authors worked around by hand. A
correct 2.0 view has **zero lifecycle code**: the shipped helper greets the
runtime itself on every document (`osfui.hello`), so first open, F5, dev
hot-reload and crash recovery are one sequence — `ready`, then a full state
replay, then events. If you catch yourself writing "on ready, re-request my
data", the value you want is state.

What the break costs:

- A view whose manifest declares `targetVersion` below `2.0` still loads, but
  every helper member it calls (`emit`, `call`, `action`, `viewReady`, `data.*`)
  was removed, so it paints nothing. OSF UI raises a `compat.legacy-view` entry
  in System Health naming the view — a blank page is the one failure a player
  cannot diagnose.
- A native plugin built against ABI 1.x gets `nullptr` from
  `OSFUI_RequestBridge` (majors must match) and raises `compat.legacy-api`
  naming its DLL. Recompile against `sdk/OSFUI_API.h`; ABI 2.0 also adds
  `SetViewState`, which is what lets a plugin publish state instead of
  hand-rolling reload handling.
- Papyrus keeps its names. Only `PushToView`, `PushFormsToView` and the
  `RegisterForViewActions*` family are gone, replaced by `SetView*` (state) and
  the new `SendViewEvent` (events).

Debugging is F12 Chromium DevTools, not a bespoke inspector: every failure an
author can cause is printed to the view's own console with an `[osfui]` prefix,
and `localStorage["osfui:trace"] = "1"` (then reload) logs every envelope in
both directions.

The rationale, including what each removed alias was papering over, is in
[docs/mod-api-2.0-design.md](docs/mod-api-2.0-design.md); the typed reference is
[`sdk/osfui.d.ts`](sdk/osfui.d.ts).

## Documentation

- [docs/authoring-settings.md](docs/authoring-settings.md) - **start here to add settings to your mod**: one JSON file, no code — quickstart, widgets, hotkeys, presets, localization, testing
- [docs/view-toolchain.md](docs/view-toolchain.md) - **start here to build a view**: scaffold, browser HMR, in-game sync, checks, and packaging
- [docs/authoring-views.md](docs/authoring-views.md) - view manifest and bridge protocol reference
- [docs/authoring-dynamic-data.md](docs/authoring-dynamic-data.md) - state vs. events: feeding a view live game data from Papyrus or a plugin, and surviving reload
- [docs/native-plugin-api.md](docs/native-plugin-api.md) - the C ABI for SFSE plugins (`sdk/OSFUI_API.h`)
- [docs/mod-api-2.0-design.md](docs/mod-api-2.0-design.md) - why the 2.0 API is shaped the way it is, and what each 1.x alias was hiding
- [frontend/README.md](frontend/README.md) - **start here to change a built-in view**: the Vite/TS/Preact source that generates `build/frontend/views/`
- [docs/architecture.md](docs/architecture.md) - layers and data flow
- [docs/security-model.md](docs/security-model.md)
- [docs/troubleshooting.md](docs/troubleshooting.md) - requirements, install, troubleshooting, uninstall, and known limitations
- [OSF Web Services](https://github.com/ozooma10/osf-web-services) - independently deployed reporting service and future OSF websites/APIs

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

Every valid view folder under `views/<modId>/<viewName>/` is discovered at boot
and loads the first time it is opened — there is no list to maintain. Which
HUDs start with the game is a per-HUD **player** setting ("Start automatically"
on the HUD's row in Mod Settings); a HUD manifest's `openOnStart` only sets the
default. (`configVersion` 1's `views`/`warmViews` keys are ignored with a
warning.)

With `devMode` enabled, saved changes to a loaded view's files (HTML/JS/CSS)
auto-reload it in place within about half a second — the fast view-iteration
loop. Press **F12** while a menu is open to inspect that view in WebView2's
native Edge DevTools.

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
