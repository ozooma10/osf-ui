# Changelog

Notable changes to OSF UI. Versions are `kPluginVersion` (`src/core/Version.h`);
the native bridge ABI (`sdk/OSFUI_API.h`) and the web bridge protocol version
independently and are called out per entry.

## 0.2.0 — 2026-07-15 — first public release

The first public build of OSF UI: a working, schema-driven overlay framework
for Starfield. It renders HTML/CSS/JS views offscreen (Ultralight/WebKit),
composites them over the game through an `IDXGISwapChain::Present` hook, and
drives them with a hardware cursor, keyboard, and gamepad while the game is
input-frozen. Ships a hub launcher, a multi-mod **Settings (MCM)** panel, and a
visual **Keybinds** rebinder; other SFSE plugins can host their own views and
settings through a native bridge API.

Requires Starfield **1.16.244** on Steam + SFSE. Web bridge protocol **0.4**
(0.x = unstable, may break views before 1.0); native plugin ABI **1.4**.

### Framework & UI

- **Hub launcher** (F10) — a catalog of installed views; opens Settings,
  Keybinds, and any third-party view.
- **Settings (MCM):** each mod drops a `settings/<id>.json` schema; a two-pane
  panel renders typed controls (bool/int/float/enum/string/key), validates,
  clamps, and persists sparsely with debounced write-behind. Live search,
  per-setting reset, presets, migration across schema versions, and dev-time
  schema hot-reload.
- **Keybinds view** — a visual keyboard map with inline rebind; key-conflict
  badges against other mods and the game's own (vanilla) bindings; `key`
  settings can be deliberately unbound (`allowUnbound`) or scoped to a named
  input context (`blocksGameplay` suppresses expected game-key overlap).
- **Hotkey service** — every `key`-typed setting is a live, gated hotkey
  delivered to its owner (native `SubscribeHotkey` or the `ui.hotkey` web push).
- A shared design system so built-in and third-party views look like one product.

### Overlay & input (engine integration)

- **Menu-mode participation:** OSF UI registers a real engine `IMenu`
  (`focusMenu`) so the game enters menu mode; the input-enable layer
  (`disableControls`) freezes keyboard, mouse-look, **and gamepad/XInput**
  (fixing the gamepad-leak); `SimPause` pauses the sim per-view; `FreeCursor`
  releases the OS pointer. All on by default and verified in-game on 1.16.244.
- **Hardware cursor** — the real Windows pointer, zero-lag and
  framerate-independent, with CSS `cursor` mapped to system cursors.
- **Gamepad navigation** (basic) — D-pad/stick move focus, A activates, B
  closes, right-stick scrolls; pages can take raw gamepad via `osfui.gamepadRaw`.
- **"MOD SETTINGS" pause-menu entry** (`pauseMenuEntry`) — injected into the
  game's pause menu via live Scaleform manipulation (no SWF edit, no conflict
  with UI-overhaul mods).
- **Resilience:** URL crash-recovery (bounded reloads then teardown), a runtime
  layout guard that refuses UI integration on a mismatched game build, and
  Present-hook coexistence diagnostics.
- Dirty-rect frame pipeline — only changed regions repaint and upload.

### Native bridge (for SFSE plugin authors)

- Exported `OSFUI_RequestBridge` → `IOSFUIBridge` (`sdk/OSFUI_API.h`): register
  bridge commands, push messages to views, `RequestMenu`, typed setting getters,
  `SubscribeSettings`, `RegisterSettingsSchema`, and (ABI 1.4) `SubscribeHotkey`.
- **ABI 1.3 delivery guarantee:** `SendToWeb` to a loaded view is queued until
  the view can receive it and delivered **before its first visible paint**, so a
  consumer can open a view directly in a target state
  (`SendToWeb(v, mode); RequestMenu(v, true);`) with no flash of the page's
  default face. Native→web queues are bounded (oldest dropped, logged); sends to
  an unknown or non-`nativeBridge` view warn once. Detect via
  `(GetInterfaceVersion() & 0xFFFF) >= 3`.
- Web bridge protocol **0.4** adds the `ui.hotkey` push and key-conflict data.

### Known limitations (v0.x)

- Steam only (SFSE limitation); not Xbox/Game Pass.
- No IME / CJK composition. Gamepad navigation is basic. No localization
  pipeline.
- HDR / 10-bit output is detected and **skipped** (SDR only) — a log warning
  names the format; full HDR output is on the roadmap.
- Coexistence with ReShade / RTSS / Steam overlay / frame-gen is untested
  (diagnostics land in the log to self-identify a broken hook chain).
- The web bridge protocol is 0.x and may break views until it reaches 1.0.

## 0.1.0

Baseline: Ultralight offscreen renderer + D3D12 present-hook compositor, menu/
HUD surface policy (focus, capture, sim pause, control layer), settings module
(MCM) with sparse write-behind persistence, native plugin API through ABI 1.2
(commands, sends, ready, `RequestMenu`, typed setting getters,
`SubscribeSettings`, `RegisterSettingsSchema`).
