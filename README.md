# StarfieldWebUI

**Starfield Web UI Runtime** — a prototype SFSE/CommonLibSF plugin that will
eventually host HTML/CSS/JS-based UI views inside Starfield (conceptually
inspired by Prisma UI for Skyrim; contains no Prisma code).

Current state: **Phase 1 complete** — real HTML/CSS/JS pages render offscreen
inside the game via Ultralight (CPU surface, dedicated worker thread,
sandboxed filesystem, two-way JSON bridge). Nothing draws **over** the game
yet — frames go to a null compositor until the D3D12 work (Phases 2–3) is
reverse-engineered: see [What this is not yet](#what-this-is-not-yet).

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
| `startVisible` | `false` | initial overlay visibility state |
| `renderer` | `"mock"` | `null` \| `mock` \| `ultralight` (real offscreen backend; shipped config uses it — falls back to `null` with a warning in Ultralight-free builds) |
| `compositor` | `"null"` | `null` \| `d3d12` (stub that refuses to init) |
| `inputSource` | `"none"` | `none` \| `ui` — observe-only vfunc hook on the game UI's input processing; enables the toggle key. Shipped config uses `ui`; set to `none` to rule the hook out when debugging |
| `view` | `"test"` | view id from `views/*/manifest.json` |
| `allowNetwork` | `false` | recognized but force-disabled |
| `devMode` | `true` | verbose per-call logging |

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
  created on the game's own `ID3D12Device`, submitted on its direct command
  queue (route reverse-engineered and QI-verified at runtime; hook-free).
  Verified in-game with a byte-exact GPU round-trip check. Still no drawing
  over the game — that is Phase 3.
- The JSON message bridge parses/dispatches the whitelisted commands
  (`close`, `log`, `ping`, `setVisible`) and rejects everything else.
- The sample `test` view is a self-contained HTML panel that also runs
  standalone in a normal browser (degraded mode) for development.

## What this is not yet

- **Not a complete Prisma port** — and it will never contain Prisma code
  unless explicitly provided and licensed.
- **Not an MCM** — schema-driven settings UI is Phase 5 of
  [docs/renderer-plan.md](docs/renderer-plan.md).
- **Not a working in-game browser overlay** — `Runtime::Tick()` runs every
  frame via SFSE's TaskInterface, but nothing draws over the game; the
  required render/input integration points are deliberately unimplemented
  until they are properly reverse engineered
  ([docs/reverse-engineering-notes.md](docs/reverse-engineering-notes.md)).
- **Not compatible with Xbox/Game Pass** — SFSE itself is Steam-only.

## Documentation

- [docs/architecture.md](docs/architecture.md) — layers and data flow
- [docs/renderer-plan.md](docs/renderer-plan.md) — Phases 0–5
- [docs/security-model.md](docs/security-model.md) — JS-is-untrusted rules
- [docs/reverse-engineering-notes.md](docs/reverse-engineering-notes.md) —
  what is unknown and must not be guessed

## License

GPL-3.0 (inherited from the template). See `LICENSE` and `EXCEPTIONS`.
