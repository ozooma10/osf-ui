# Adding settings to your mod

> **Disclaimer:** This document is AI-generated (written with Claude and
> reviewed against the source code). If it ever disagrees with the code, the
> JSON Schema ([§12](#12-reference)) or `sdk/osfui.d.ts`, those are
> authoritative — and a bug report about the mismatch is welcome.

One JSON file gives your mod a full settings page in the OSF UI Mods menu
(F10): typed controls, validation, persistence, hotkey rebinding, presets,
and localization — **no code required**. This page is the complete author
guide for that file.

> Looking for custom UI instead? Full web views are covered in
> [authoring-views.md](authoring-views.md). Settings and views share the same
> mod id and compose freely — most mods ship settings first.

---

## 1. Five-minute quickstart

1. Copy [`examples/settings-only/yourname.mymod.json`](../examples/settings-only/yourname.mymod.json)
   into your mod as:

   ```
   Data\SFSE\Plugins\OSFUI\settings\<author>.<modname>.json
   ```

2. Rename it. The filename stem **is** your mod id and must equal the `"id"`
   field inside: `"<author>.<modname>"` — lowercase `a-z 0-9 -` segments,
   exactly one dot, where `author` is your Nexus/GitHub handle
   (e.g. `astrogal.compass-tweaks`). Dotless ids are reserved for the
   platform.

3. Edit `title` and the `groups` array to declare your settings.

4. Launch the game, press F10 — your card is in the left rail. Values the
   user changes persist to
   `Data\SFSE\Plugins\OSFUI\settings\values\<id>.json` (VFS-captured, so
   per-profile under MO2) and survive relaunch.

You can iterate without launching Starfield at all — see
[§10 Testing](#10-testing-your-schema).

A minimal real schema:

```jsonc
{
  "$schema": "https://github.com/ozooma10/osf-ui/blob/main/docs/schema/settings-schema.schema.json",
  "id": "yourname.mymod",
  "title": "My Mod",
  "version": 1,
  "groups": [
    {
      "label": "General",
      "settings": [
        { "key": "enabled", "label": "Enabled", "type": "bool", "default": true },
        { "key": "opacity", "label": "HUD opacity", "type": "float",
          "min": 0, "max": 1, "step": 0.05, "default": 0.8,
          "format": { "scale": 100, "suffix": "%", "decimals": 0 } }
      ]
    }
  ]
}
```

The `$schema` line gives you autocomplete and inline validation in VS Code
and other editors. Use it.

---

## 2. Setting types

Every setting is `{ "key", "type", "default", ... }` inside a group.
Validation happens natively on every write — out-of-range values are clamped,
wrong types rejected. This is the frozen 1.0 type set:

| `type` | Control | Value & validation |
|---|---|---|
| `bool` | toggle switch | `true` / `false` |
| `int` | slider (or stepper) | number, clamped to `[min, max]`, rounded; `step` is the UI granularity |
| `float` | slider (or stepper) | number, clamped to `[min, max]` |
| `enum` | dropdown (or segmented) | one of `options` (required) |
| `flags` | checkbox group | array of `options` strings — multi-select; unknowns/duplicates filtered, order canonicalized |
| `string` | text field (or textarea, or color swatch) | truncated to 256 chars (`maxLength` tightens the UI limit) |
| `key` | press-to-rebind button | a key name like `"F8"`; see [§7 Hotkeys](#7-hotkeys) |

There is **no `color` type** — use `"type": "string", "widget": "color"`
(stored as `"#rrggbb"` / `"#rrggbbaa"`).

Common per-setting fields:

- `label` — control label (defaults to `key`); `hint` — helper text under it.
- `default` — the initial value, also the reset target. The "modified" dot in
  the UI compares against it, so pick real defaults.
- `optionLabels` — display strings parallel to `options`, so stored values
  stay machine-stable: `"options": ["off","min","full"], "optionLabels":
  ["Off","Minimal","Full"]`. Stored values are never translated; labels are.

### Widgets and number formatting

`"widget"` picks an alternate control for the same type — older hosts ignore
it safely:

| Type | Widgets |
|---|---|
| `int` / `float` | `slider` (default), `stepper` |
| `enum` | `dropdown` (default), `segmented` (best for 2–4 options) |
| `string` | `text` (default), `textarea`, `color` |

`"format"` turns raw numbers into friendly display strings while storing the
clean value:

```jsonc
{ "key": "opacity", "type": "float", "min": 0, "max": 1, "default": 0.85,
  "format": { "scale": 100, "suffix": "%", "decimals": 0 } }   // shows "85%"
```

`prefix` / `suffix` / `scale` (display multiplier) / `decimals`.

---

## 3. Groups and show/hide rules

Groups are ordered sections: `{ "id", "label", "collapsed", "visibleWhen",
"settings": [...] }`. Give groups a stable `id` if you ever expect
translations (it survives reordering). With many groups the host renders a
section index automatically; `"collapsed": true` starts one folded.

**Conditions** show/hide (`visibleWhen`) rows and whole groups, or
enable/disable (`enabledWhen`) individual rows, based on sibling settings of
the *same mod*:

```jsonc
{ "key": "compass.size", "type": "float", "min": 0.5, "max": 2, "default": 1,
  "visibleWhen": { "key": "compass.enabled", "eq": true },
  "enabledWhen": { "all": [
    { "key": "mode", "in": ["compact", "full"] },
    { "not": { "key": "scale", "lt": 75 } }
  ] } }
```

Leaf operators: `eq ne in gt gte lt lte truthy`. Combinators: `all any not`.
A reference to an unknown key evaluates false. Conditions are **display
sugar only** — a hidden setting is still writable via the bridge and still
natively validated, so never rely on hiding for correctness.

### Restart badges

```jsonc
{ "key": "backend", "type": "enum", "options": ["auto","gpu","cpu"],
  "default": "auto", "requires": "restart" }   // "restart" | "reload" | "newGame"
```

The row gets a badge for any of the three values; for `"restart"` a banner
additionally aggregates all pending restart-required changes.

---

## 4. Notes, images, and action buttons

Besides value settings, a group can contain static and interactive rows:

```jsonc
{ "type": "note", "id": "dlc-note", "style": "info",
  "text": "Requires **Shattered Space**. See the *tuning guide*." }

{ "type": "image", "src": "assets/preview.png", "caption": "Compact layout", "height": 120 }

{ "type": "action", "key": "recalibrate", "label": "Run calibration",
  "command": "yourname.mymod.recalibrate",
  "style": "accent",                                   // "default" | "accent" | "danger"
  "confirm": "Clear learned data and recalibrate now?",
  "enabledWhen": { "key": "enabled", "eq": true } }
```

- **Notes** support micro-markdown only: `**bold**`, `*italic*`, `` `code` ``,
  `\n`. No HTML, no links — everything renders injection-safe.
- **Image** `src` is relative to your `views/<id>/` folder (ship one even if
  it only holds assets); no `..`, absolute paths, or URL schemes.
- **Actions** fire a bridge command that **must** be namespaced
  `<your-id>.something` — the host refuses anything else. Handle it in your
  SFSE plugin via `RegisterCommand` ([native-plugin-api.md](native-plugin-api.md))
  and optionally reply `{ "type": "yourname.mymod.ack", "payload": { "key",
  "ok", "message"? } }` to resolve the button (it times out after 5 s
  otherwise).

---

## 5. Presets

Author-shipped value sets, applied as a batch of ordinary validated writes.
Partial maps are fine — unlisted keys are untouched:

```jsonc
"presets": [
  { "id": "performance", "label": "Performance", "description": "Lightweight HUD",
    "values": { "hud.mode": "compact", "hud.opacity": 0.6 } },
  { "id": "cinematic", "label": "Cinematic",
    "values": { "hud.mode": "full", "hud.opacity": 1.0 } }
]
```

---

## 6. Branding the card

Top-level, both optional:

- `"accent": "#7a9a5e"` — tints your detail pane (otherwise the default
  OSF UI accent is used).
- `"icon": "badge.svg"` — path inside `views/<id>/`, shown in the rail and
  launcher cards instead of the initials monogram. SVG or PNG, drawn at
  ~30–52 px square.

---

## 7. Hotkeys

Every `"type": "key"` setting is a **live, rebindable hotkey** — you never
write input-hook code:

```jsonc
{ "key": "toggleHud", "label": "Toggle HUD", "type": "key", "default": "F8" }
```

- The user rebinds it by pressing the button in your card (capture happens in
  the native input layer, so even the overlay toggle key itself is
  rebindable).
- When the bound key is pressed in-game, OSF UI dispatches it to you — see
  [§8](#8-reading-your-settings-consumption) for the web and C++ delivery.
  Dispatch happens **only during gameplay**: it is suppressed while any game
  menu is open (pause menu, inventory, dialogue, main menu, …) and while the
  overlay is capturing input, so you never double-handle typing or react to
  presses the player made inside a menu.
- Conflicts with other mods or Starfield's own bindings show as
  **informational warnings** — never blocked, both mods still fire.
- `"allowUnbound": true` permits `""` as a deliberate unbound state (adds an
  unbind × in the UI; unbound keys never dispatch and never warn).

If your mod suppresses normal gameplay controls during a modal state (a
scene, a minigame), declare an input context so intentional reuse of gameplay
keys (Space, E, …) doesn't warn:

```jsonc
"inputContexts": [
  { "id": "scene", "label": "During scenes", "blocksGameplay": true }
],
"groups": [{ "settings": [
  { "key": "progressScene", "type": "key", "default": "Space", "inputContext": "scene" }
] }]
```

`blocksGameplay` is an author assertion — only use it when the game's
bindings genuinely cannot fire while your context is active. It suppresses
`@game` warnings only; mod-to-mod collisions still warn.

---

## 8. Reading your settings (consumption)

The schema stores values; making them *do* something is your mod's half.
Pick the surface where your logic lives:

### From your own web view (zero native code)

One call reads everything **and** subscribes you to live changes and hotkeys:

```js
osfui.on("settings.changed", (p) => {
  if (p.mod === "yourname.mymod") applySetting(p.key, p.value);
});
osfui.on("ui.hotkey", (p) => {
  if (p.mod === "yourname.mymod" && p.key === "toggleHud") toggle();
});
osfui.send("settings.get");   // initial read + subscription in one call
```

Values arrive post-validation (authoritative). Full protocol reference:
[authoring-views.md §3–4](authoring-views.md).

### From an SFSE plugin (C++)

Fetch the bridge from [`sdk/OSFUI_API.h`](../sdk/OSFUI_API.h) (settings
getters and `SubscribeSettings` need ABI ≥ 1.2, `SubscribeHotkey` needs
ABI ≥ 1.4; see [native-plugin-api.md](native-plugin-api.md)):

```cpp
// Typed getters — synchronous, callable from any thread.
bool enabled = false;
g_bridge->GetSettingBool("yourname.mymod", "enabled", &enabled);

// Change subscription — fired on the game main thread, and REPLAYED once per
// current value on subscribe, so you need no separate initial read.
g_bridge->SubscribeSettings("yourname.mymod",
    [](const char* mod, const char* key, const char* valueJson, void* user) noexcept {
        // switch on key; valueJson is the JSON-encoded value
    }, nullptr);

// Hotkeys — fired on the game main thread when the bound key is pressed.
g_bridge->SubscribeHotkey("yourname.mymod", "toggleHud",
    [](const char* mod, const char* key, void* user) noexcept { /* toggle */ }, nullptr);
```

A DLL can also skip the drop-in file and register at runtime with
`RegisterSettingsSchema(json)` — same JSON document, same values file, so a
mod can move between the two without users losing settings. If both exist,
the DLL registration wins (with a logged warning).

### From Papyrus

An esm+scripts mod needs **no DLL and no registration call**: the drop-in
schema file above *is* the registration, and the shipped `OSFUI` script
(`Data/Scripts/OSFUI.pex`, source + full API docs in
`Data/Scripts/Source/OSFUI.psc`) reads it back:

```papyrus
; Feature-detect: 0 => OSF UI absent (natives unbound; every call below then
; yields the default you pass). Packed major*10000 + minor*100 + patch.
If OSFUI.GetVersion() >= 10000   ; needs 1.0.0+
    Float scale = OSFUI.GetFloat("yourname.mymod", "hud.scale", 1.0)
EndIf
```

- **Getters** — `GetBool` / `GetInt` / `GetFloat` / `GetString(modId, key,
  default)`: cheap, thread-safe reads of the live value store. Unknown
  mod/key or a type mismatch yields the passed default. `GetString` covers
  string-, enum-, and key-typed settings. Ids, keys, and enum option values
  match your schema case-insensitively (Papyrus string interning can't
  preserve casing, so OSF UI folds it); write them as authored anyway.
- **Setters** — `SetBool` / `SetInt` / `SetFloat` / `SetString(modId, key,
  value)` and `Reset(modId, key = "")`: fire-and-forget; the write is
  validated/clamped against your schema and persisted through exactly the
  same path as the settings menu (refusals are logged to `OSF UI.log`). An
  open settings card updates live.
- **Change events + hotkeys** — register a callback; hotkeys are just your
  `"type": "key"` settings (§7), so the user sees and rebinds them in the
  menu while OSF UI owns the input hook and dispatches presses to you:

```papyrus
ScriptName MyModQuest Extends Quest

Function RegisterAll()
    OSFUI.RegisterForSettingChanges(self as ScriptObject, "OnSettingChanged", "yourname.mymod")
    OSFUI.RegisterForHotkey(self as ScriptObject, "OnHotkey", "yourname.mymod", "toggleHud")
EndFunction

Function OnSettingChanged(string modId, string key)
    ; fires after ANY writer commits (menu, native, Papyrus) — re-read via getters
EndFunction

Function OnHotkey(string modId, string key)
    ; gameplay-only delivery: never fires while the user types in an overlay
    ; or rebinds a key, and never consumes the press
EndFunction
```

Registrations are **session-scoped** (they do not survive a save load) —
call your `RegisterAll()` from quest init *and* every game load (e.g.
`OnPlayerLoadGame` on a player `ReferenceAlias`). `RegisterFor*` returns a
token for `Unregister(token)`; `...Static` variants dispatch to global
functions on a named script for library-style mods. `OpenMenu()` opens the
Mods surface (same as F10) for a "configure" shortcut.

Settings cover pre-declared scalars. For **dynamic data** — pushing live
lists/tables to a view of your own and reacting to its clicks, all from
Papyrus — see [authoring-dynamic-data.md](authoring-dynamic-data.md)
(`PushToView` / `RegisterForViewActions`).

---

## 9. Updating your mod

- **Declare `"version": 1`** from day one (a plain integer you bump on
  meaningful schema changes; it's stamped into values files as
  `$schemaVersion` for diagnostics). Never name a setting key with a leading
  `$` — those are reserved host meta keys.
- **Renaming a key:** keep the old name as an alias; saved values migrate on
  the next load, no version arithmetic:

  ```jsonc
  { "key": "hud.opacity", "aliases": ["opacity"], "type": "float", ... }
  ```

- **Changing a default:** just change it. Persistence is sparse (only values
  the user actually changed are written), so users who never touched the
  knob get your new default automatically.
- **Changing a type:** old saved values that no longer validate fall back to
  the new default. Prefer a new key with an alias when the meaning changes.
- **Using features newer than the OSF UI you tested on:** declare
  `"targetVersion": "1.1.0"` (the OSF UI version you authored against). The
  schema still loads best-effort on older hosts; unknown decorations are
  ignored, unknown types render read-only, and the Mods surface shows a
  "needs update" badge naming your mod. Saved values for unknown types are
  preserved untouched for newer hosts.
- **Uninstall:** the values file is deliberately kept (MO2 profile switches
  look identical to uninstalls). Reinstalling restores the user's settings.

---

## 10. Testing your schema

**Browser harness — no game launch.** Open
[`devtools/harness/index.html`](../devtools/harness/index.html) in a browser
(or run `serve.cmd`) and drag your JSON onto the page (or pass
`?schema=<path>`). It renders the *real* settings view
with a mock bridge that mirrors native clamping and persists to
localStorage, and logs the exact bridge traffic. Widgets, conditions,
presets, actions, and rebinding all work there.

**Editor validation.** The `$schema` line covers most mistakes as you type.
For CI or a final check:

```
npx ajv-cli validate --spec=draft2020 -s docs/schema/settings-schema.schema.json -d yourname.mymod.json
```

**In-game hot reload.** With `"devMode": true` in OSF UI's `config.json`,
saved changes to `settings\*.json` are picked up within ~1 s while the game
runs — values survive, the open menu repaints. F11 (configurable
`devReloadKey`) reloads the open view.

**Broken files are loud, not silent.** A bad filename or unparseable JSON is
skipped and reported (with line/column) in an alert pinned atop the Mods
rail; a corrupt values file is quarantined to `<id>.json.bad` and defaults
served. If your card doesn't appear, look there first, then at `OSF UI.log`.

---

## 11. Localization

Write plain English in your schema — no string keys, nothing extra to
maintain. Translators (you, or the community, as a separate data mod) ship:

```
Data\SFSE\Plugins\OSFUI\l10n\<id>_<locale>.json     e.g. yourname.mymod_de.json
```

A flat map from structural addresses to translated text; partial files are
fine, and the authored English is the fallback for every missing entry:

```json
{
  "settings.title": "Mein Mod",
  "settings.hud.enabled.label": "HUD aktivieren",
  "settings.hud.mode.options.compact": "Kompakt",
  "groups.general.label": "Allgemein"
}
```

Addresses derive from your stable identities — setting keys, stored option
values, and the optional `id` on groups/presets/notes/images (give those an
`id` so translations survive reordering; the array index is the fallback).
Mod ids, setting keys, stored `options`, and commands are never localized.
See [`examples/settings-only/l10n/`](../examples/settings-only/l10n/) for a
worked pair.

---

## 12. Reference

- **Formal schema (autocomplete + validation):**
  [`docs/schema/settings-schema.schema.json`](schema/settings-schema.schema.json)
- **Copy-me template exercising every widget:**
  [`examples/settings-only/`](../examples/settings-only/)
- **Bridge protocol (messages, payloads, TypeScript types):**
  [authoring-views.md](authoring-views.md), [`sdk/osfui.d.ts`](../sdk/osfui.d.ts)
- **C ABI for SFSE plugins:** [native-plugin-api.md](native-plugin-api.md),
  [`sdk/OSFUI_API.h`](../sdk/OSFUI_API.h)
- **Design rationale (internal RFC, not an author guide):**
  [mcm-design.md](mcm-design.md)
