# StarfieldWebUI

**Starfield Web UI Runtime** — a prototype SFSE/CommonLibSF plugin that will
eventually host HTML/CSS/JS-based UI views inside Starfield (conceptually
inspired by Prisma UI for Skyrim; contains no Prisma code).

Current state: **Phase 0** — a clean, buildable architecture with logging,
config, view manifests, a narrow JSON message bridge, and isolated
renderer/compositor interfaces. It does **not** draw anything in-game yet, on
purpose: see [What this is not yet](#what-this-is-not-yet).

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
  `<mods>/StarfieldWebUI/SFSE/Plugins/...`
- `XSE_SF_GAME_PATH` — the game folder → installs to `Data/SFSE/Plugins/...`

The install includes the DLL, PDB, and the `StarfieldWebUI/` data folder
(config + views).

## Install / paths

Final layout (game or mod folder):

```
Data/SFSE/Plugins/
  StarfieldWebUI.dll
  StarfieldWebUI/              <- plugin data, resolved relative to the DLL
    config.json
    views/
      test/
        manifest.json
        index.html  style.css  main.js
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
| `renderer` | `"mock"` | `null` \| `mock` \| `ultralight` (stub) |
| `compositor` | `"null"` | `null` \| `d3d12` (stub that refuses to init) |
| `inputSource` | `"none"` | `none` \| `ui` — observe-only vfunc hook on the game UI's input processing; enables the toggle key. Shipped config uses `ui`; set to `none` to rule the hook out when debugging |
| `view` | `"test"` | view id from `views/*/manifest.json` |
| `allowNetwork` | `false` | recognized but force-disabled |
| `devMode` | `true` | verbose per-call logging |

## Optional: Ultralight backend

The default build has **zero** Ultralight footprint and must stay that way.
To compile the (currently stub) Ultralight renderer:

```bat
set ULTRALIGHT_SDK_DIR=C:\path\to\ultralight-sdk
xmake f --with_ultralight=true
xmake build
```

The build fails with a clear message if `ULTRALIGHT_SDK_DIR` is missing. The
SDK is proprietary — never commit its headers, libs, or binaries to this
repository (also mind Ultralight's own licensing terms for distribution).

## What works today

- Plugin loads under SFSE and logs its full lifecycle (preload, load, SFSE
  broadcast messages).
- `Runtime::Tick()` runs every frame on the game's main thread via an SFSE
  `TaskInterface` permanent task (heartbeat logged in dev mode).
- Menu open/close events are observed via the documented CommonLibSF
  `RegisterSink` API; with `inputSource: "ui"`, keyboard/mouse buttons are
  observed through an isolated, pass-through vfunc hook and the configured
  toggle key flips overlay visibility (pending in-game verification).
- Config and view manifests load defensively from the plugin data path.
- Renderer/compositor backends are selected from config with safe fallbacks;
  the mock renderer produces a real CPU RGBA test pattern, the null
  compositor logs submitted frames.
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
