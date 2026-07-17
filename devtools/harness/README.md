# OSF UI — panel dev harness

Iterate on the shipped panels (the Mods surface, keybinds — and settings
schemas) in a normal browser, without launching Starfield. Each page loads the
**real** shipped view assets (`data/OSFUI/views/osfui/<view>/main.js` + CSS) behind a
`MockBridge` that speaks the bridge protocol — `settings.get/set/reset/captureKey`
with the same validation and clamping rules as the native `SettingsStore`, plus
the Mods surface's `views.get` / `menu.open` / `hud.show|hide` and the
localization read `i18n.get` / `i18n.data` (see "Test localization" below) —
persists to `localStorage`, and logs every message to the console.

## Run it

Double-click `serve.cmd` (or serve `c:\Modding\Starfield` — one level **above**
the repo — over http yourself) and the Mods harness opens in the browser:

```sh
cd c:\Modding\Starfield
python -m http.server 8080
# then open "http://localhost:8080/OSF UI/devtools/harness/index.html"
```

The serve root is the parent dir so the harness can also reach sibling-repo
views (OSF Animation's `osf` view ships from its own repo — in game the two
merge through MO2's VFS, and no copy lives here).

Pages (the top bar cross-links them):

| Page | View under test |
| --- | --- |
| `index.html` | Mods surface — the overlay front door (schemas fetched from `data/OSFUI/settings/`, catalog views mocked) |
| `keybinds.html` | Keybinds keyboard map |
| `osf.html` | OSF Animation scene browser — the **real** view from `..\OSF Animation\views\osf\`, in an iframe. It self-mocks (no MockBridge): built-in catalog, scan, anchor match, emote wheel (`W` / `Shift+W`). The 1280×720 button shows it at the in-game surface size. |

Opening a page from `file://` also works — it just falls back to built-in
sample data instead of fetching the shipped schemas.

The Mods and Keybinds pages have a **1600×900** top-bar toggle that renders
the view in a game-true 900p stage — the manifest (logical) resolution the
views are authored against — scaled to fill the browser window exactly the
way the game scales it to screen (`device_scale = outputHeight / 900`, so a
1080p window shows it at the in-game 1.2×). In this mode the harness's
bar-clearing margins are dropped and the view uses its shipped in-game
layout, so what you see is the exact in-game composition and text size (the
dashed outline marks the screen edge). This is the default; add `?res=off`
to the URL (or click the toggle) for the old fluid fill-the-window mode.
OSF Animation's page has the equivalent **1280×720** toggle — that view's
actual in-game surface size.

Style refinement loop: edit `data/OSFUI/views/osfui/<view>/style.css` or
`views/shared/osfui.css`, then refresh the page (Ctrl+Shift+R to be safe) —
the harness always reads the live repo files, so what you refine here is
exactly what ships. **Note:** each harness page carries a *copy* of its view's
`index.html` body markup (the shipped page can't include the mock script) — if
you change a view's HTML structure, mirror it in the harness page.

## Catalog specifics (panels + HUDs on the Mods surface)

By default the mock catalog holds just the real views (Mods, Keybinds, OSF
Animation Browser). The **Sample views** top-bar button (or `?fixtures=1`,
persisted in `localStorage`) adds fictional fixtures so every state the Mods
surface renders is exercised: views owned by a settings mod (manifest `mod`
matches a schema id — they appear on that mod's page), view-only mods (no
schema — their own rail entry), a failed load, and HUD live/hidden. Clicking
**Open** on Keybinds or the OSF Animation Browser
navigates to its harness page, so panel launch behaves like the in-game
single-menu swap; a fictional panel just marks itself open. HUD toggles flip
live.

## Load your schema (settings / keybinds pages)

- **Drag-drop:** drop one or more `settings/<author>.<modname>.json` files onto the page.
- **Query param:** `…/devtools/harness/?schema=../../path/to/mymod.json`.
- By default it loads the shipped `osfui` schema, plus OSF Animation's `osf`
  schema — that one is registered natively in game (`RegisterSettingsSchema`),
  so the mock extracts the same `R"json(...)"` literal from
  `..\OSF Animation\src\API\UISettings.cpp`.

Use **Reset stored values** (top bar) to clear the mock's `localStorage` and
return every mod to its schema defaults.

## Test localization (settings / keybinds pages)

The **Locale** picker in the top bar (or `?locale=…`, persisted in
`localStorage`) drives a mirror of the runtime's localization path: the mock
answers `i18n.get` with `i18n.data` like `Runtime` does, localizes settings
schemas at the same structural addresses as the native `LocalizeSchema`
(`settings.<key>.label`, `groups.<id>.label`, …), localizes view catalog
titles/descriptions (`views.<name>.title` / `.description`), and re-pushes all
three on a locale switch like `RefreshLocalizedData`.

Two modes:

- **`pseudo`** — no catalog needed. Every string that goes through the
  localization path is transformed: letters accented (glyph coverage), ~30%
  length padding (German-ish expansion), and `[brackets]` around the whole
  string. Anything still showing plain English is **hardcoded** — it never
  went through `osfui.t` / `data-i18n` / schema localization — and anything
  overflowing its box is a layout that won't survive a long translation.
- **A real locale** (e.g. `de`) — applies l10n catalogs: the same flat
  `<modId>_<locale>.json` address→string files the game loads from
  `SFSE/Plugins/OSFUI/l10n/`. Drop one onto the page (it wins over any
  fetched file, so re-drop to iterate on a translation live; dropping while
  the locale is `en` auto-activates the file's locale), or ship it in the
  repo's `data/OSFUI/l10n/` / `examples/settings-only/l10n/`, which the mock
  fetches per loaded mod. Fallback merges base language under the exact
  locale (`pt-BR` ← `pt`), like the native `FallbackLocales`; addresses not
  in the catalog fall back to authored English, like in game.

`en` is "authored" — localization off, the pristine default. The vanilla key
titles (`Starfield (Quicksave)` …) stay authored in the harness; in game they
re-localize with everything else.

## What it verifies

Everything the renderer does client-side: widgets, `visibleWhen`/`enabledWhen`
conditions, number formatting, colour input, notes, action buttons (the mock
simulates a `<mod>.ack` reply after 400 ms), presets, modified-dots, per-setting
and per-mod reset, global search, the undo panel (visit-scoped revert), the
save-state indicator (the mock mirrors native write-behind: one
`settings.persisted` push ~500 ms after a change window opens), and the
catalog's live open/focus/load states.

It does **not** exercise native consumption (a mod reacting to a value),
Papyrus, or Ultralight-specific rendering differences — those need the game.
This is a layout/style/schema iteration tool.
