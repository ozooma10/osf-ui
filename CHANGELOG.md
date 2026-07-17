# Changelog

Notable changes to OSF UI. Versions are `kPluginVersion` (`src/core/Version.h`);
the native bridge ABI (`sdk/OSFUI_API.h`) and the web bridge protocol version
independently and are called out per entry.

## 1.0.0 — 2026-07-17 — initial public alpha

The first published build of OSF UI: a working, schema-driven overlay framework
for Starfield. It renders HTML/CSS/JS views offscreen (Ultralight/WebKit),
composites them over the game through an `IDXGISwapChain::Present` hook, and
drives them with a hardware cursor, keyboard, and gamepad while the game is
input-frozen. Ships a unified **Mods surface** with a multi-mod
**Settings (MCM)** panel and a visual **Keybinds** rebinder; other SFSE
plugins can host their own views and settings through a native bridge API.

Requires Starfield **1.16.244** on Steam + SFSE. Web bridge protocol **0.4**
(0.x = unstable, may break views before it reaches 1.0); native plugin ABI
**1.5**. This is an **alpha**: the plugin version starts the 1.x line, but the
web bridge protocol deliberately stays 0.x until it stabilizes.

### Framework & UI

- **The Mods surface** (F10) — a single per-mod menu: the left rail lists every
  installed mod (the union of settings schemas and registered catalog views);
  a mod's page shows its terminal launch buttons and HUD toggles above its
  settings controls. Openable full-screen views are called **terminals** in
  the UI (overlays stay overlays).
- **Home — the launcher page.** Every fresh visit to the Mods surface lands on
  a pinned Home entry at the top of the rail: an app-style card grid of every
  registered terminal (click to open — Keybinds included) and overlay toggle
  across all mods. `#mod=<id>` deep links win over the default landing.
- **Settings (MCM):** each mod drops a `settings/<id>.json` schema; a two-pane
  panel renders typed controls (bool/int/float/enum/string/key), validates,
  clamps, and persists sparsely with debounced write-behind. Live search,
  per-setting reset, presets, migration across schema versions, and dev-time
  schema hot-reload. Shipped and example schemas declare `"version": 1`;
  new mods should start there so migration stamping is active from day one.
- **Keybinds view** — a visual keyboard map with inline rebind; key-conflict
  badges against other mods and the game's own (vanilla) bindings; `key`
  settings can be deliberately unbound (`allowUnbound`) or scoped to a named
  input context (`blocksGameplay` suppresses expected game-key overlap).
- **Hotkey service** — every `key`-typed setting is a live, gated hotkey
  delivered to its owner (native `SubscribeHotkey` or the `ui.hotkey` web push).
- **A shared design system, styled to sit next to the game's own menus:**
  flat translucent blue-black panels, hairline rules, hard rectangular edges,
  a desaturated HUD ice-blue accent (`#9fc7dc`), the Constellation tricolor
  motif, and hover/selection that inverts to white like the game's menu
  entries. Per-mod accent theming keeps third-party views on-brand.

### Authoring surface

- **View manifest: optional `mod` field** — the owning settings mod id. The
  Mods surface groups a view onto that mod's page; views without it get their
  own rail entry (`views.data` carries `mod` per view).
- **Settings schema: optional `icon` presentation field** — a badge image
  (path inside the mod's own view folder, same sandbox as `image` rows) drawn
  in the Mods surface rail and Home cards in place of the title-initials
  monogram. A missing or rejected file falls back to initials. The dev harness
  resolves these paths through `OSFUI_MOD_ASSET_ROOTS` in mockbridge.js (in
  game, mod view folders are siblings of the settings view; under
  devtools/harness they are not).

### Overlay & input (engine integration)

- **Menu-mode participation:** OSF UI registers a real engine `IMenu`
  (`focusMenu`) so the game enters menu mode; the input-enable layer
  (`disableControls`) freezes keyboard, mouse-look, **and gamepad/XInput**;
  `SimPause` pauses the sim per-view; `FreeCursor` releases the OS pointer.
  All on by default and verified in-game on 1.16.244.
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
  `SubscribeSettings`, `RegisterSettingsSchema`, (ABI 1.4) `SubscribeHotkey`,
  and (ABI 1.5) `RegisterView`.
- **ABI 1.5 `RegisterView`:** a plugin that ships a `views/<id>/` folder makes
  it an openable surface (Mods-surface catalog + `RequestMenu` + web
  `menu.open`) with one call — the user's `config.json` no longer has to list
  third-party views in `views`. Idempotent; `RegisterView` → `SendToWeb` →
  `RequestMenu` back-to-back land in call order on one tick.
- **ABI 1.3 delivery guarantee:** `SendToWeb` to a loaded view is queued until
  the view can receive it and delivered **before its first visible paint**, so a
  consumer can open a view directly in a target state
  (`SendToWeb(v, mode); RequestMenu(v, true);`) with no flash of the page's
  default face. Native→web queues are bounded (oldest dropped, logged); sends to
  an unknown or non-`nativeBridge` view warn once. Detect via
  `(GetInterfaceVersion() & 0xFFFF) >= 3`.
- Web bridge protocol **0.4** adds the `ui.hotkey` push and key-conflict data.

### Known limitations (alpha)

- Steam only (SFSE limitation); not Xbox/Game Pass.
- No IME / CJK composition. Gamepad navigation is basic. No localization
  pipeline.
- HDR / 10-bit output is detected and **skipped** (SDR only) — a log warning
  names the format; full HDR output is on the roadmap.
- Coexistence with ReShade / RTSS / Steam overlay / frame-gen is untested
  (diagnostics land in the log to self-identify a broken hook chain).
- The web bridge protocol is 0.x and may break views until it reaches 1.0.

> Note for pre-release testers: the shipped payload no longer includes the
> `demo.json` schema or Demo Mod / My Mod preview data, and `config.json` now
> ships `devMode: false` — remove `Data/SFSE/Plugins/OSFUI/settings/demo.json`
> from an old loose-file install or reinstall clean. The built-in `hub` view is
> removed; shipped `config.json` uses `"view": "settings"`,
> `"views": ["settings", "keybinds"]`.

## 0.2.0 — 2026-07-15 — internal milestone (not published)

Feature-complete snapshot 1.0.0 was cut from; it still shipped the hub
launcher that the Mods surface replaced. Its contents are folded into the
1.0.0 notes above.

## 0.1.0 — internal baseline

Ultralight offscreen renderer + D3D12 present-hook compositor, menu/
HUD surface policy (focus, capture, sim pause, control layer), settings module
(MCM) with sparse write-behind persistence, native plugin API through ABI 1.2
(commands, sends, ready, `RequestMenu`, typed setting getters,
`SubscribeSettings`, `RegisterSettingsSchema`).
