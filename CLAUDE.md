# CLAUDE.md

Guidance for AI assistants (and humans) working in this repository. Read this
first, then `docs/architecture.md` and `docs/reverse-engineering-notes.md`
before touching anything that integrates with the game engine.

## What this project is

**OSF UI** is an SFSE / CommonLibSF plugin for **Starfield** (Steam, Direct3D 12)
that hosts HTML/CSS/JS UI views over the running game. Real web pages render
offscreen via **Ultralight** (WebKit), upload to a GPU texture on the game's own
D3D12 device, and draw as an alpha-blended overlay through an
`IDXGISwapChain::Present` hook. The overlay is interactive: it freezes the game,
routes keyboard/mouse into the page, and shows the real Windows hardware cursor.
A schema-driven, multi-mod settings UI (MCM-style) ships on top.

- Language: **C++23**, built with **XMake**. License **GPL-3.0** (inherited from
  `commonlibsf-template`). Windows-only, MSVC / Clang-CL.
- It is **inspired by [Prisma UI](https://www.prismaui.dev/)** (Skyrim) but is an
  independent, from-scratch implementation and **contains no Prisma UI code**.
  Do not add Prisma names, branding, or API.
- Naming convention (deliberate, keep it consistent):
  - repo folder == XMake target == MO2 mod folder == **`OSF UI`** (with a space)
  - compiled binary == **`OSFUI.dll`** (no space)
  - plugin data folder == **`SFSE/Plugins/OSFUI/`** (`kDataFolderName` in
    `src/core/Version.h`, *not* derived from the DLL name)
  - C++ namespace == `OSFUI`, JS bridge global == `window.osfui`

## Build & CI

```bat
xmake build                      :: default: ZERO Ultralight footprint
```

Output lands in `build/windows/x64/<mode>/`. Modes: `debug`, `releasedbg`
(configure with `xmake f -m <mode>`). Cold builds also compile the CommonLibSF
submodule; incremental builds are fast.

**Auto-deploy** (set before configuring): `XSE_SF_MODS_PATH` → installs to
`<mods>/OSF UI/SFSE/Plugins/...`, or `XSE_SF_GAME_PATH` → `Data/SFSE/Plugins/...`.

**Ultralight (real renderer) build** — the SDK is **proprietary and must NEVER
be committed** (keep local drops under the gitignored `external/`):

```bat
set ULTRALIGHT_SDK_DIR=C:\path\to\ultralight-sdk
xmake f --with_ultralight=true
xmake build
```

The build fails with a clear message if `ULTRALIGHT_SDK_DIR` is missing.

### Tests / lint

- **There is no unit-test suite and no separate linter.** "Correctness" for the
  game-integration layers is verified in-game on a real Starfield install
  (see `docs/HANDOFF.md` smoke test). You almost certainly cannot run the game
  here; make the smallest correct change and reason about it carefully.
- **CI** (`.github/workflows/ci.yml`, Windows) does exactly three things on the
  **zero-Ultralight** target: (1) validates that every shipped JSON under
  `data/` and `docs/schema/` parses; (2) `xmake build` for both modes;
  (3) stages the install layout and asserts the required files exist **and that
  the DLL has zero Ultralight footprint** (no `ultralight/` folder, no
  `Ultralight.dll` string in the binary). The proprietary SDK never enters CI.
- Before you consider a change done: keep all shipped JSON valid, and never let
  the default build gain an Ultralight dependency.

## Repository layout

```
src/
  main.cpp        SFSE entry macros -> Plugin::OnPreLoad / OnLoad
  core/           Plugin (entry glue, SFSE messages), Config, Paths, Log, Version
  runtime/        Runtime (composition root — owns everything),
                  ViewManager + ViewManifest, MessageBridge (feature-agnostic
                  command dispatcher), Json, MenuController,
                  UiModule (IUiModule contract), SettingsModule + SettingsStore
  render/         IWebRenderer + Null / Mock / Ultralight backends
  composite/      ICompositor + Null / D3D12 (present-time overlay) + EngineD3D12
  input/          InputRouter, MenuEventSink, OverlayInputHook (WndProc subclass),
                  HardwareCursor, FreeCursor, ControlLayer, EngineInput,
                  FocusMenu, SimPause, UiLayoutGuard
  api/            Native plugin API impl (BridgeApi, Exports) — pairs with sdk/
  platform/       WindowsPlatform (isolated Win32 calls)
data/OSFUI/       shipped at runtime next to the DLL
  config.json                          runtime config (see below)
  settings/<id>.json                   per-mod settings schemas
  views/<id>/{manifest.json,index.html,style.css,main.js}   hub, settings, test
  views/shared/osfui.css               shared view styles
sdk/              OSFUI_API.h (native C++ API, single header), osfui.d.ts (view TS defs)
docs/             architecture, renderer-plan, security-model,
                  reverse-engineering-notes, authoring-views, native-plugin-api,
                  HANDOFF, ROADMAP, troubleshooting, schema/*.schema.json
tools/            package.ps1 (release zips), parse_versionlib.py
packaging/        Nexus page assets / branding (not built)
lib/commonlibsf   git submodule (recursive; nests commonlib-shared)
```

Clone recursively — submodules are required: `git clone --recurse-submodules`.

## Architecture (how it fits together)

`Runtime` (`src/runtime/Runtime.cpp`) is the **composition root**: on
`SFSE_PLUGIN_LOAD` it wires paths → config → views → renderer → compositor →
bridge → input, and owns everything. An SFSE permanent task calls
`Runtime::Tick(dt)` every frame on the game's main thread.

Per-frame data flow: `IWebRenderer::Update(dt)` advances the page →
`Render()` returns a **non-owning** `FrameBufferView` of CPU pixels (valid only
until the next renderer call) → `ICompositor::Submit(frame)` consumes it
immediately (copy/upload), never stores it.

Backends sit behind stable interfaces so the rest of the runtime never knows
which is active:
- Renderers (`IWebRenderer`): **Null** (safe fallback), **Mock** (real CPU RGBA
  test pattern, no SDK), **Ultralight** (real, offscreen, `with_ultralight` only).
- Compositors (`ICompositor`): **Null** (logs frames), **D3D12** (real
  present-time overlay).

Failures **degrade loudly, never crash** — a missing/broken backend falls back to
Null with an ERROR log.

### Message bridge

All native↔web traffic is JSON `{ "type": string, "payload": object }` through
`MessageBridge`, which is **feature-agnostic**: web→native accepts only
`ui.command`, dispatched through a handler registry (`RegisterCommand`); there is
**no generic "call native" escape hatch**. Platform commands (`close`,
`setVisible`, `log`, `ping`, `menu.*`, `hud.*`, `game.get`, `views.get`) are
registered by core; each feature module registers its own namespace (e.g.
`settings.get`/`set`/`reset`). Native→web uses `SendToWeb(type, payload)`.

### Feature modules & views

Features are `IUiModule`s (`runtime/UiModule.h`); `Runtime::BuildModules()` is the
only place that names concrete modules and injects dependencies. Today there is
one module: `SettingsModule`. Views are discovered by `ViewManager` scanning
`data/OSFUI/views/*/manifest.json`; a manifest declares id, entry page, size,
transparency, and a **deny-by-default** permission block (`nativeBridge`,
`filesystem`, `network`). Manifest entries may not point outside the view folder.

## Non-negotiable conventions

These are the rules the codebase is built around. Violating them is how you break
the game or the security model.

1. **Never guess engine internals.** No invented addresses, offsets, vtable
   slots, or menu names. Every engine integration point is reverse-engineered and
   runtime-proven *before* use. If you need a new one, it does not exist until
   it's proven — see `docs/reverse-engineering-notes.md`. The one vtable ID in
   use comes from CommonLibSF, not from us.
2. **Layout guard before UI integration.** `UiLayoutGuard` refuses all UI
   integration if the compiled CommonLibSF layout doesn't match the running
   binary (a mismatch previously caused a save-load crash). This live-vptr-vs-
   AddressLib check is the pattern for every layout-dependent integration.
3. **Hooks are one-way.** The `Present` hook and the WndProc subclass install
   once and are never un-hooked (other overlays may chain on top). "Disable" uses
   a pass-through flag, not an unhook.
4. **JS is untrusted.** Views are mod content from the internet. Parse bridge
   input defensively (non-throwing JSON, typed accessors with defaults,
   length-bounded logging), enforce the command whitelist, never eval or reflect,
   and force network/filesystem off. `settings.set` may only write a key that
   exists in the schema, clamped to its declared type/range. See
   `docs/security-model.md` and honor every rule there.
5. **The Ultralight SDK is never vendored.** The default build must always
   compile and run with **zero** Ultralight footprint. All Ultralight code is
   `#if OSFUI_WITH_ULTRALIGHT`-guarded (and `UltralightWebRenderer.cpp` is
   excluded from the default build in `xmake.lua`).
6. **Threading discipline.** All Ultralight/WebCore calls live on one dedicated
   worker thread (WebKit is thread-affine). All D3D12 GPU work happens on the
   render thread inside the Present hook; the SFSE tick thread only memcpys a
   frame into a lock-guarded staging buffer. `Runtime::Tick` runs under SFSE's
   queue lock, so keep it cheap.
7. **No shutdown path.** SFSE has no shutdown callback; `Runtime::Shutdown()`
   exists but is unreachable. State must tolerate teardown by process exit.
8. Keep code in the style of its neighbors: tabs for indentation, the existing
   `OSFUI` namespace layout, and matching comment density. There is no
   `.clang-format`.

## Runtime config

`data/OSFUI/config.json` is the shipped runtime config; built-in fallbacks live
in `src/core/Config.h` (used when the file is missing). Key fields: `renderer`
(`null`|`mock`|`ultralight`), `compositor` (`null`|`d3d12`), `inputSource`
(`none`|`ui`), `captureInput`, `hardwareCursor`, `focusMenu`, `disableControls`,
`toggleKey` (default `F10`), `view` + `views` (multi-view set; the active one
must be in `views`; missing ids are skipped with a log line), and `devMode`
(verbose logging + first-frame PNG dump). The README has the full table. For
development, set `devMode: true` and `startVisible: true`.

Bump `kBridgeProtocolVersion` in `src/core/Version.h` on any breaking bridge
change, and keep it in lockstep with `docs/authoring-views.md`, `docs/schema/*`,
and `sdk/osfui.d.ts`.

## Git workflow

- Develop on the designated feature branch; create it locally if needed. Push
  with `git push -u origin <branch>`. Do **not** push to other branches, and do
  **not** open a PR unless explicitly asked.
- Several sibling workspace repos are intentionally dirty — never run
  `git checkout --` / `reset` / `stash` on files you did not modify.
- `build/`, `vs*/`, `dist/`, `external/`, and `*.zip` are gitignored and
  regenerable.

## Where to look next

- `docs/architecture.md` — layers and data flow (authoritative).
- `docs/reverse-engineering-notes.md` — what is unknown and must not be guessed.
- `docs/HANDOFF.md` — deep implementation facts (tick source, input, Ultralight
  threading, D3D12 compositor internals) and the in-game smoke test.
- `docs/security-model.md` — the JS-is-untrusted rules (enforce all of them).
- `docs/authoring-views.md` — building a view: manifest, bridge protocol,
  settings schema format.
- `docs/native-plugin-api.md` + `sdk/OSFUI_API.h` — the (new, no-shipping-
  consumer-yet) native SFSE plugin bridge API.
- `docs/renderer-plan.md` / `docs/ROADMAP.md` — phase log and remaining work.
