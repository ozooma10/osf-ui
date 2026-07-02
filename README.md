# OSF UI

[![CI](https://github.com/ozooma10/osf-ui/actions/workflows/ci.yml/badge.svg)](https://github.com/ozooma10/osf-ui/actions/workflows/ci.yml)

**OSF UI** is an SFSE/CommonLibSF plugin that hosts HTML/CSS/JS UI views over
Starfield. It is **heavily inspired by [Prisma UI](https://www.prismaui.dev/)
by StarkMP** — the Skyrim framework that pioneered rendering modern web UIs over
a Bethesda game with Ultralight — but it is an independent, from-scratch
implementation for Starfield's Direct3D 12 engine, with its own name, API, and
architecture, and contains **no Prisma UI code** (see
[Credits & acknowledgments](#credits--acknowledgments)).

Current state: **Phase 5b — interactive overlay + schema-driven settings.**
Real HTML/CSS/JS pages render offscreen via Ultralight, upload to a GPU
texture on the game's own D3D12 device, and are drawn as an alpha-blended
overlay over the live game image through an `IDXGISwapChain::Present` slot-8
hook. The overlay is **fully interactive**: while it is open the game is
input-frozen (movement + camera), keyboard routes into the page, and the
real Windows **hardware cursor** (zero-lag, framerate-independent) clicks
the page's controls, with CSS `cursor` styles mapped to the matching system
cursor. A multi-mod, schema-driven settings UI (MCM-style) ships on top. All
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

The install includes the DLL, PDB, and the `OSFUI/` data folder
(config + views).

## Install / paths

Final layout (game or mod folder):

```
Data/SFSE/Plugins/
  OSFUI.dll
  OSFUI/                 <- plugin data, resolved relative to the DLL
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
(`Documents/My Games/Starfield/SFSE/Logs/OSF UI.log`).

## Config

`OSFUI/config.json` (missing file ⇒ built-in defaults, logged):

| field | default | meaning |
|---|---|---|
| `enabled` | `true` | master switch |
| `toggleKey` | `"F10"` | key name resolved to a Windows virtual-key code by a built-in table (F1–F24, letters/digits, and named keys like `Tab`/`Escape`; unresolvable names disable the toggle with a log warning) |
| `focusKey` | `"Tab"` | cycles the active (input) view when more than one *interactive* view is hosted; passes through normally otherwise |
| `consoleKey` | `"Grave"` | the game's console key. While the overlay captures input this key is passed straight through to the game (and dismisses the overlay so the console isn't left hidden behind it). `Grave` = VK_OEM_3 (grave/tilde) on US layouts — retarget for other layouts/rebinds; empty string disables the pass-through |
| `startVisible` | `false` | initial overlay visibility state |
| `renderer` | `"mock"` | `null` \| `mock` \| `ultralight` (real offscreen backend; shipped config uses it — falls back to `null` with a warning in Ultralight-free builds) |
| `compositor` | `"null"` | `null` \| `d3d12` — the real overlay path (uploads to the game's D3D12 device, draws at present time; verified in-game). Shipped config uses `d3d12` |
| `inputSource` | `"none"` | `none` \| `ui` — installs the window-subclass input path (toggle key + input capture). Shipped config uses `ui`; set to `none` to rule the input hook out when debugging |
| `captureInput` | `true` | when the overlay is visible, freeze the game and route keyboard/mouse into the web view (needs `inputSource: "ui"`). When `false` the overlay is a passive HUD: it draws, but the game still receives input |
| `hardwareCursor` | `true` | show the real Windows (hardware) pointer while the overlay captures input — zero-lag, framerate-independent, and the page's CSS `cursor` maps to the matching system cursor. `false` falls back to the legacy raw-delta virtual cursor, which has **no visible pointer** (debugging escape hatch only) |
| `focusMenu` | `false` (shipped config: `true`) | **Experimental.** Register a real engine menu (`OSFUI_FocusMenu`) and open/close it with the overlay so the engine enters menu mode (cursor + modal input) rather than relying only on the WndProc message-swallow. Custom-`IMenu` registration + open are **proven on 1.16.244**; the hardened headless menu's *long-run* survival is the remaining risk (see `src/core/Config.h` and [docs/reverse-engineering-notes.md](docs/reverse-engineering-notes.md) §4). Set `false` if you see instability with the overlay open |
| `disableControls` | `false` (shipped config: `true`) | While the overlay is visible, disable player controls through the engine input-enable layer (`BSInputEnableManager`) — this also stops gamepad/XInput, which the WndProc hook never sees. **Proven live on 1.16.244 with a controller** (keyboard, mouse-look, and gamepad sticks all freeze and restore cleanly) |
| `view` | `"test"` | the active (input) view — a view id from `views/*/manifest.json` (shipped config uses `settings`) |
| `views` | `[]` | optional multi-view set: every id is loaded and composited (layer order = each view's manifest `zorder`), and `view` must be one of them (the interactive one). Empty ⇒ only `view` loads. Missing ids are skipped with a log line. Shipped config uses `["settings", "hud"]` |
| `allowNetwork` | `false` | recognized but force-disabled |
| `devMode` | `false` | verbose per-call logging + first-frame PNG dump. **Defaults shown above are the built-in fallbacks (`src/core/Config.h`); the shipped `data/OSFUI/config.json` is the runtime config.** For development, set `devMode: true` and `startVisible: true` for verbose logs and a launch-visible overlay |

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
`OSFUI/ultralight/` folder shown above. The SDK is proprietary —
never commit its headers, libs, or binaries to this repository (keep local
drops under the gitignored `external/`; also mind Ultralight's own licensing
terms for distribution).

CI (GitHub Actions) builds and layout-checks the **zero-Ultralight** target
only — the proprietary SDK never enters CI. Release archives that bundle the
Ultralight runtime (and its required license notices) are produced locally
with `tools/package.ps1` (see [docs/PACKAGING.md](docs/PACKAGING.md)).

Implementation notes (threading model, delay-load bootstrapping, JS bridge,
sandbox) live in [docs/renderer-plan.md](docs/renderer-plan.md) Phase 1 and
docs/HANDOFF.md §4.

## What works today

- Plugin loads under SFSE and logs its full lifecycle (preload, load, SFSE
  broadcast messages).
- `Runtime::Tick()` runs every frame on the game's main thread via an SFSE
  `TaskInterface` permanent task (heartbeat logged in dev mode).
- Menu open/close events are observed via the documented CommonLibSF
  `RegisterSink` API; with `inputSource: "ui"`, a WndProc subclass on the game
  window drives the configured toggle key and input capture (verified in-game
  2026-06-12). A runtime layout guard (`UiLayoutGuard`) refuses all UI
  integration if the compiled engine layout doesn't match the running binary.
- Config and view manifests load defensively from the plugin data path.
- Renderer/compositor backends are selected from config with safe fallbacks;
  the mock renderer produces a real CPU RGBA test pattern, the null
  compositor logs submitted frames.
- With `with_ultralight`: real HTML/CSS/JS rendering offscreen (Ultralight
  1.4 / WebKit on a dedicated worker thread), a sandboxed filesystem limited
  to the view folder + ICU resources, and a two-way `window.osfui`
  JSON bridge — verified in-game (devMode dumps the first rendered frame to
  `OSFUI/ultralight/first-frame.png`).
- With `compositor: "d3d12"`: the rendered frames upload into a GPU texture
  on the game's own `ID3D12Device` (route reverse-engineered and QI-verified
  at runtime; hook-free) and are **drawn over the game image** at present
  time via an `IDXGISwapChain::Present` slot-8 hook — alpha-blended,
  user-verified on screen. `startVisible`/F10 toggle it.
- **Interactive input** (`inputSource: "ui"` + `captureInput: true`): a
  WndProc subclass on the game window freezes gameplay while the overlay is
  open, routes keyboard (VK codes) into the focused page, and shows the real
  Windows **hardware cursor** to click the page's controls — zero-lag and
  framerate-independent, with CSS `cursor` styles (hover hand, text I-beam)
  mapped onto the OS pointer. F10 toggles, Esc closes.
- **Schema-driven settings (MCM-style):** each mod drops a
  `settings/<id>.json` schema (typed bool/int/float/enum/string knobs); the
  built-in `settings` view renders a card per mod with live controls + Reset,
  and the native `SettingsStore` validates/clamps/persists per-mod to a
  user-writable path and fires change reactions (e.g. live cursor speed).
- The JSON message bridge parses/dispatches the whitelisted commands —
  surface control (`close`, `setVisible`, `menu.open` / `menu.close`,
  `hud.show` / `hud.hide`, `setViewHidden`), diagnostics (`log`, `ping`),
  read-only game data (`game.get`), the surface catalog (`views.get` — replies
  with every loaded view's metadata + open/focus/load state and pushes updates
  on change, the read behind a hub/launcher view), and the settings trio
  (`settings.get` / `settings.set` / `settings.reset`) — and rejects everything
  else. Trusted
  *native* SFSE plugins can register additional commands through the exported
  bridge API ([docs/native-plugin-api.md](docs/native-plugin-api.md));
  untrusted JS cannot. Authoring a view? See
  [docs/authoring-views.md](docs/authoring-views.md).
- The sample `test` and `settings` views are self-contained HTML panels that
  also run standalone in a normal browser (degraded mode) for development.

## What this is not yet

- **A young framework** — multiple views composite together with `Tab` focus
  switching, and views can read basic game data (e.g. the in-game clock).
  Still maturing: the `window.osfui` declarative bridge is **0.x and
  unstable**, the DevTools inspector is stubbed, the native plugin API
  (`sdk/OSFUI_API.h` / `OSFUI_RequestBridge` — lets a separate SFSE plugin
  register bridge commands and drive views,
  [docs/native-plugin-api.md](docs/native-plugin-api.md)) is brand-new with no
  shipping consumer yet, and there's no packaging/distribution tooling for
  third-party UIs yet. You ship a `settings/<id>.json` schema and a
  `views/<id>/` folder for the declarative path. Render/menu integration points
  were reverse engineered before use, never guessed
  ([docs/reverse-engineering-notes.md](docs/reverse-engineering-notes.md)).
- **Rough edges** — text entry follows the OS keyboard layout (dead keys and
  AltGr included, verified in-game) but IME composition (e.g. CJK input) is
  not supported yet, there is no gamepad/controller navigation or
  localization pipeline,
  HDR/10-bit backbuffers are detected and skipped with a log warning rather
  than rendered (full HDR output is on the roadmap), and coexistence with
  ReShade / Steam overlay / frame-gen is untested (see
  [docs/renderer-plan.md](docs/renderer-plan.md) Phases 3 & 4c).
- **Not compatible with Xbox/Game Pass** — SFSE itself is Steam-only.

## Documentation

- [docs/troubleshooting.md](docs/troubleshooting.md) — **players start here**:
  requirements, install, troubleshooting, uninstall, and known limitations
- [docs/authoring-views.md](docs/authoring-views.md) — **start here to build
  a view**: package layout, manifest fields, the bridge protocol, and the
  settings schema format
- [docs/architecture.md](docs/architecture.md) — layers and data flow
- [docs/renderer-plan.md](docs/renderer-plan.md) — Phases 0–5
- [docs/security-model.md](docs/security-model.md) — JS-is-untrusted rules
- [docs/reverse-engineering-notes.md](docs/reverse-engineering-notes.md) —
  what is unknown and must not be guessed

## Credits & acknowledgments

OSF UI exists because of **[Prisma UI](https://www.prismaui.dev/)** by
**StarkMP** (with contributors incl. **langfod**) — the Skyrim Special Edition
framework that pioneered rendering modern HTML/CSS/JS interfaces over a Bethesda
game with Ultralight. The entire idea for this project came from Prisma UI, and
StarkMP graciously gave permission to build a Starfield-flavored framework in
its spirit. Thank you.

OSF UI is an **independent, from-scratch implementation for Starfield** — a
different game on a different engine (Direct3D 12 vs Prisma's Direct3D 11), with
its own name, architecture, renderer, input handling, and native↔web bridge. It
does **not** use Prisma UI's name, branding, or API, is **not affiliated with or
endorsed by the Prisma UI project, and contains no Prisma UI code.**

- **[Prisma UI](https://www.nexusmods.com/skyrimspecialedition/mods/148718)** —
  StarkMP & contributors — original concept and inspiration.
- **[Ultralight](https://ultralig.ht/)** — Ultralight, Inc. — the lightweight,
  WebKit-based renderer behind every view (used under the Ultralight Free
  License; notices ship in `OSFUI/ultralight/license/`).
- **[commonlibsf-template](https://github.com/libxse/commonlibsf-template)** /
  **CommonLibSF** & **[SFSE](https://sfse.silverlock.org/)** — the plugin
  foundation this is built on.

See [CREDITS.md](CREDITS.md) for the full text, including a paste-ready blurb
for mod pages.

## License

GPL-3.0 (inherited from the template). See `LICENSE` and `EXCEPTIONS`.
