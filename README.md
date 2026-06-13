# StarfieldWebUI

**Starfield Web UI Runtime** — a prototype SFSE/CommonLibSF plugin that will
eventually host HTML/CSS/JS-based UI views inside Starfield (inspired by
[Prisma UI](https://www.prismaui.dev/) by StarkMP — see
[Credits & acknowledgments](#credits--acknowledgments); contains no Prisma
code).

Current state: **Phase 5b — interactive overlay + schema-driven settings.**
Real HTML/CSS/JS pages render offscreen via Ultralight, upload to a GPU
texture on the game's own D3D12 device, and are drawn as an alpha-blended
overlay over the live game image through an `IDXGISwapChain::Present` slot-8
hook. The overlay is **fully interactive**: while it is open the game is
input-frozen (movement + camera), keyboard routes into the page, and a
virtual cursor driven by raw mouse input clicks the page's controls. A
multi-mod, schema-driven settings UI (MCM-style) ships on top. All
user-verified on Starfield 1.16.244. See
[docs/renderer-plan.md](docs/renderer-plan.md) for the full phase log and
[What this is not yet](#what-this-is-not-yet) for the remaining gaps.

Based on [commonlibsf-template](https://github.com/libxse/commonlibsf-template)
(GPL-3.0).

## Requirements

- [XMake](https://xmake.io) 3.0.0+
- C++23 compiler (MSVC / Clang-CL)
- Starfield (Steam) + [SFSE](https://sfse.silverlock.org/)

## Build

```bat
xmake build
```

Output lands in `build/windows/x64/<mode>/`. To deploy automatically, set one
of (before configuring):

- `XSE_SF_MODS_PATH` — a mod manager `mods` folder → installs to
  `<mods>/OSF UI/SFSE/Plugins/...` (mod folder == xmake target == repo folder)
- `XSE_SF_GAME_PATH` — the game folder → installs to `Data/SFSE/Plugins/...`

The install includes the DLL, PDB, and the `StarfieldWebUI/` data folder
(config + views).

## Install / paths

Final layout (game or mod folder):

```
Data/SFSE/Plugins/
  OSF UI.dll
  StarfieldWebUI/              <- plugin data, resolved relative to the DLL
                                  (name is the kDataFolderName constant, not the DLL name)
    config.json
    views/
      test/
        manifest.json
        index.html  style.css  main.js
    ultralight/                <- only present in with_ultralight builds
      bin/                        (delay-loaded at runtime, preloaded by the plugin)
        Ultralight.dll  UltralightCore.dll  WebCore.dll  AppCore.dll
      resources/
        icudt67l.dat            (ICU data; cacert.pem deliberately not shipped)
```

Logs go to the standard SFSE log folder
(`Documents/My Games/Starfield/SFSE/Logs/StarfieldWebUI.log`).

## Config

`StarfieldWebUI/config.json` (missing file ⇒ built-in defaults, logged):

| field | default | meaning |
|---|---|---|
| `enabled` | `true` | master switch |
| `toggleKey` | `"F10"` | key name resolved via SFSE InputMap (F1–F12 layout-independent, other names layout-dependent) |
| `focusKey` | `"Tab"` | cycles the active (input) view when more than one *interactive* view is hosted; passes through normally otherwise |
| `startVisible` | `false` | initial overlay visibility state |
| `renderer` | `"mock"` | `null` \| `mock` \| `ultralight` (real offscreen backend; shipped config uses it — falls back to `null` with a warning in Ultralight-free builds) |
| `compositor` | `"null"` | `null` \| `d3d12` (stub that refuses to init) |
| `inputSource` | `"none"` | `none` \| `ui` — observe-only vfunc hook on the game UI's input processing; enables the toggle key. Shipped config uses `ui`; set to `none` to rule the hook out when debugging |
| `captureInput` | `true` | when the overlay is visible, freeze the game and route keyboard/mouse into the web view (needs `inputSource: "ui"`). When `false` the overlay is a passive HUD: it draws, but the game still receives input |
| `view` | `"test"` | view id from `views/*/manifest.json` (shipped config uses `settings`) |
| `allowNetwork` | `false` | recognized but force-disabled |
| `devMode` | `false` | verbose per-call logging + first-frame PNG dump. **Defaults shown above are the built-in fallbacks (`src/core/Config.h`); the shipped `data/StarfieldWebUI/config.json` is the runtime config.** For development, set `devMode: true` and `startVisible: true` for verbose logs and a launch-visible overlay |

## Ultralight backend

The default build has **zero** Ultralight footprint and must stay that way.
To compile the real renderer:

```bat
set ULTRALIGHT_SDK_DIR=C:\path\to\ultralight-sdk
xmake f --with_ultralight=true
xmake build
```

The build fails with a clear message if `ULTRALIGHT_SDK_DIR` is missing, and
the install step ships the SDK's runtime DLLs + ICU data into the
`StarfieldWebUI/ultralight/` folder shown above. The SDK is proprietary —
never commit its headers, libs, or binaries to this repository (keep local
drops under the gitignored `external/`; also mind Ultralight's own licensing
terms for distribution).

Implementation notes (threading model, delay-load bootstrapping, JS bridge,
sandbox) live in [docs/renderer-plan.md](docs/renderer-plan.md) Phase 1 and
docs/HANDOFF.md §4.

## What works today

- Plugin loads under SFSE and logs its full lifecycle (preload, load, SFSE
  broadcast messages).
- `Runtime::Tick()` runs every frame on the game's main thread via an SFSE
  `TaskInterface` permanent task (heartbeat logged in dev mode).
- Menu open/close events are observed via the documented CommonLibSF
  `RegisterSink` API; with `inputSource: "ui"`, keyboard/mouse buttons are
  observed through an isolated, pass-through vfunc hook and the configured
  toggle key flips overlay visibility (verified in-game 2026-06-12).
- Config and view manifests load defensively from the plugin data path.
- Renderer/compositor backends are selected from config with safe fallbacks;
  the mock renderer produces a real CPU RGBA test pattern, the null
  compositor logs submitted frames.
- With `with_ultralight`: real HTML/CSS/JS rendering offscreen (Ultralight
  1.4 / WebKit on a dedicated worker thread), a sandboxed filesystem limited
  to the view folder + ICU resources, and a two-way `window.starfield`
  JSON bridge — verified in-game (devMode dumps the first rendered frame to
  `StarfieldWebUI/ultralight/first-frame.png`).
- With `compositor: "d3d12"`: the rendered frames upload into a GPU texture
  on the game's own `ID3D12Device` (route reverse-engineered and QI-verified
  at runtime; hook-free) and are **drawn over the game image** at present
  time via an `IDXGISwapChain::Present` slot-8 hook — alpha-blended,
  user-verified on screen. `startVisible`/F10 toggle it.
- **Interactive input** (`inputSource: "ui"` + `captureInput: true`): a
  WndProc subclass on the game window freezes gameplay while the overlay is
  open, routes keyboard (VK codes) into the focused page, and turns raw mouse
  deltas into a virtual cursor that clicks the page's controls. F10 toggles,
  Esc closes.
- **Schema-driven settings (MCM-style):** each mod drops a
  `settings/<id>.json` schema (typed bool/int/float/enum/string knobs); the
  built-in `settings` view renders a card per mod with live controls + Reset,
  and the native `SettingsStore` validates/clamps/persists per-mod to a
  user-writable path and fires change reactions (e.g. live cursor speed).
- The JSON message bridge parses/dispatches the whitelisted commands
  (`close`, `setVisible`, `log`, `ping`, and the settings trio `settings.get`
  / `settings.set` / `settings.reset`) and rejects everything else. Authoring
  a view? See [docs/authoring-views.md](docs/authoring-views.md).
- The sample `test` and `settings` views are self-contained HTML panels that
  also run standalone in a normal browser (degraded mode) for development.

## What this is not yet

- **Not a complete Prisma port** — and it will never contain Prisma code
  unless explicitly provided and licensed.
- **Not yet a multi-mod platform** — exactly one view is active at a time
  (`config.view`), feature modules are compiled into the runtime, and there
  is no public/stable API or packaging format for third-party UIs yet. You
  *can* ship a `settings/<id>.json` schema and a `views/<id>/` folder today;
  anything more (new bridge commands, game-data-bound HUDs, separate-DLL
  modules) still needs core changes. Render/menu integration points were
  reverse engineered before use, never guessed
  ([docs/reverse-engineering-notes.md](docs/reverse-engineering-notes.md)).
- **Rough edges** — keyboard routing is US-layout only (no IME/Unicode yet),
  there is no gamepad/controller navigation or localization pipeline, and
  HDR/10-bit backbuffers plus coexistence with ReShade / Steam overlay /
  frame-gen are untested (see [docs/renderer-plan.md](docs/renderer-plan.md)
  Phases 3 & 4c).
- **Not compatible with Xbox/Game Pass** — SFSE itself is Steam-only.

## Documentation

- [docs/authoring-views.md](docs/authoring-views.md) — **start here to build
  a view**: package layout, manifest fields, the bridge protocol, and the
  settings schema format
- [docs/architecture.md](docs/architecture.md) — layers and data flow
- [docs/renderer-plan.md](docs/renderer-plan.md) — Phases 0–5
- [docs/security-model.md](docs/security-model.md) — JS-is-untrusted rules
- [docs/reverse-engineering-notes.md](docs/reverse-engineering-notes.md) —
  what is unknown and must not be guessed

## Credits & acknowledgments

StarfieldWebUI exists because of **[Prisma UI](https://www.prismaui.dev/)** by
**StarkMP** (with contributors incl. **langfod**) — the Skyrim Special Edition
framework that pioneered rendering modern HTML/CSS/JS interfaces over a Bethesda
game with Ultralight. The entire idea for this project came from Prisma UI, and
StarkMP graciously gave permission to reference Prisma UI's branding and public
API. Thank you.

This is an **independent, from-scratch implementation for Starfield** — a
different game on a different engine (Direct3D 12 vs Prisma's Direct3D 11), with
its own architecture, renderer, input handling, and native↔web bridge. It is
**not affiliated with or endorsed by the Prisma UI project, and contains no
Prisma UI code.**

- **[Prisma UI](https://www.nexusmods.com/skyrimspecialedition/mods/148718)** —
  StarkMP & contributors — original concept and inspiration.
- **[Ultralight](https://ultralig.ht/)** — Ultralight, Inc. — the lightweight,
  WebKit-based renderer behind every view (used under the Ultralight Free
  License; notices ship in `StarfieldWebUI/ultralight/license/`).
- **[commonlibsf-template](https://github.com/libxse/commonlibsf-template)** /
  **CommonLibSF** & **[SFSE](https://sfse.silverlock.org/)** — the plugin
  foundation this is built on.

See [CREDITS.md](CREDITS.md) for the full text, including a paste-ready blurb
for mod pages.

## License

GPL-3.0 (inherited from the template). See `LICENSE` and `EXCEPTIONS`.
