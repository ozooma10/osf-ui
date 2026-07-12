# OSF UI — settings dev harness

Iterate on settings schemas (and the settings view itself) in a normal browser,
without launching Starfield. It loads the **real** shipped view assets
(`data/OSFUI/views/settings/main.js` + CSS) behind a `MockBridge` that answers
`settings.get/set/reset/captureKey` with the same validation and clamping rules
as the native `SettingsStore`, persists to `localStorage`, and logs every
message to the console.

## Run it

Serve the repo root over http (needed so the mock can `fetch` the shipped
schemas) and open the harness:

```sh
cd "OSF UI"
python3 -m http.server 8080
# then open http://localhost:8080/devtools/harness/
```

Opening `index.html` from `file://` also works — it just falls back to a
built-in sample instead of fetching the shipped schemas.

## Load your schema

- **Drag-drop:** drop one or more `settings/<id>.json` files onto the page.
- **Query param:** `…/devtools/harness/?schema=../../path/to/mymod.json`.
- By default it loads the shipped `osfui`/`demo` schemas plus
  `examples/settings-only/mymod.json`.

Use **Reset stored values** (top bar) to clear the mock's `localStorage` and
return every mod to its schema defaults.

## What it verifies

Everything the renderer does client-side: widgets, `visibleWhen`/`enabledWhen`
conditions, number formatting, colour input, notes, action buttons (the mock
simulates a `<mod>.ack` reply after 400 ms), presets, modified-dots, per-setting
and per-mod reset, global search, and the session-changes revert panel.

It does **not** exercise native consumption (a mod reacting to a value) or
Papyrus — those need the game. This is a schema/UI iteration tool.
