# Adding settings to your mod

One JSON file gives your mod a full settings page in the OSF UI Mods menu (F10): typed controls, validation, persistence, hotkey rebinding, presets and localization, **no code required**. This is the complete guide to that file.

Want custom UI instead? Full web views are [authoring-views.md](authoring-views.md). Settings and views share the same mod id and compose freely — most mods ship settings first.

> Written with Claude and reviewed against the source. Where it disagrees with the code, the [JSON Schema](#12-reference) or `sdk/osfui.d.ts`, those win — and a bug report about the mismatch is welcome.

---

## 1. Quickstart

1. Copy [`examples/settings-only/yourname.mymod.json`](../examples/settings-only/yourname.mymod.json) into your mod as `Data\SFSE\Plugins\OSFUI\settings\<author>.<modname>.json`.
2. Rename it. The filename stem **is** your mod id and must equal the `"id"` inside: `"<author>.<modname>"` — lowercase `a-z 0-9 -` segments, exactly one dot, where `author` is your Nexus/GitHub handle (e.g. `astrogal.compass-tweaks`). Dotless ids are reserved for the platform.
3. Edit `title` and `groups`.
4. Launch, press F10 — your card is in the left rail. Values persist to `Data\SFSE\Plugins\OSFUI\settings\values\<id>.json` (VFS-captured, so per-profile under MO2) and survive relaunch.

You can iterate without launching Starfield — see [§10](#10-testing-your-schema).

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

The `$schema` line gives autocomplete and inline validation in VS Code and friends. Use it.

---

## 2. Setting types

Every setting is `{ "key", "type", "default", ... }` inside a group. Validation happens natively on every write: out-of-range clamped, wrong types rejected. This is the frozen 1.0 type set:

| `type` | Control | Value & validation |
|---|---|---|
| `bool` | toggle switch | `true` / `false` |
| `int` | slider (or stepper) | number, clamped to `[min, max]`, rounded; `step` is UI granularity |
| `float` | slider (or stepper) | number, clamped to `[min, max]` |
| `enum` | dropdown (or segmented) | one of `options` (required) |
| `flags` | checkbox group | array of `options` strings; unknowns/duplicates filtered, order canonicalized |
| `string` | text field (or textarea, or color swatch) | truncated to 256 chars (`maxLength` tightens the UI limit) |
| `key` | press-to-rebind button | a key name like `"F8"` — see [§7](#7-hotkeys) |

There is **no `color` type** — use `"type": "string", "widget": "color"` (stored as `"#rrggbb"` / `"#rrggbbaa"`).

Common per-setting fields:

- `label` — control label (defaults to `key`); `hint` — helper text under it.
- `default` — initial value and reset target. The UI's "modified" dot compares against it, so pick real defaults.
- `optionLabels` — display strings parallel to `options`, keeping stored values machine-stable: `"options": ["off","min","full"], "optionLabels": ["Off","Minimal","Full"]`. Stored values are never translated; labels are.

### Widgets and number formatting

`"widget"` picks an alternate control for the same type; older hosts ignore it safely.

| Type | Widgets |
|---|---|
| `int` / `float` | `slider` (default), `stepper` |
| `enum` | `dropdown` (default), `segmented` (best for 2–4 options) |
| `string` | `text` (default), `textarea`, `color` |

`"format"` displays a friendly string while storing the clean value — `prefix` / `suffix` / `scale` (display multiplier) / `decimals`:

```jsonc
{ "key": "opacity", "type": "float", "min": 0, "max": 1, "default": 0.85,
  "format": { "scale": 100, "suffix": "%", "decimals": 0 } }   // shows "85%"
```

---

## 3. Pages, groups and show/hide rules

Groups are ordered sections: `{ "id", "label", "collapsed", "page", "visibleWhen", "settings": [...] }`. Give groups a stable `id` if you expect translations (it survives reordering). With many groups the host renders a section index automatically; `"collapsed": true` starts one folded.

### Pages

When one column gets long, segment it into tabs — declare them at the top level and point groups at one:

```jsonc
{
  "pages": [
    { "id": "browser", "label": "Browser" },
    { "id": "advanced", "label": "Advanced" }
  ],
  "groups": [
    { "label": "Hotkeys",  "settings": [ /* ... */ ] },                      // no page
    { "label": "Library",  "page": "browser",  "settings": [ /* ... */ ] },
    { "label": "Logging",  "page": "advanced", "settings": [ /* ... */ ] }
  ]
}
```

- A group with no `page` (or an unknown id) lands on an implicit **General** tab, painted first — so adding pages later never hides an untagged group.
- A page no group references renders no tab; tabs appear only when content splits across at least two non-empty pages.
- Pages are display-only annotations on the flat `groups` list. A host predating them ignores both fields and renders the plain column, so a paged schema stays usable on older versions (declare `targetVersion` if you want those hosts to badge "needs update").
- Tab labels localize at `pages.<id>.label`. Section index and search still work; a search jump raises the right tab.

### Conditions

`visibleWhen` shows/hides rows and whole groups; `enabledWhen` enables/disables individual rows. Both reference sibling settings of the *same mod*:

```jsonc
{ "key": "compass.size", "type": "float", "min": 0.5, "max": 2, "default": 1,
  "visibleWhen": { "key": "compass.enabled", "eq": true },
  "enabledWhen": { "all": [
    { "key": "mode", "in": ["compact", "full"] },
    { "not": { "key": "scale", "lt": 75 } }
  ] } }
```

Leaf operators `eq ne in gt gte lt lte truthy`; combinators `all any not`. An unknown key evaluates false. Conditions are **display sugar only** — a hidden setting is still writable via the bridge and still natively validated, so never rely on hiding for correctness.

### Restart badges

```jsonc
{ "key": "backend", "type": "enum", "options": ["auto","gpu","cpu"],
  "default": "auto", "requires": "restart" }   // "restart" | "reload" | "newGame"
```

All three values badge the row; `"restart"` additionally feeds a banner aggregating pending restart-required changes.

---

## 4. Notes, images, and action buttons

A group can also contain static and interactive rows:

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

- **Notes** support micro-markdown only: `**bold**`, `*italic*`, `` `code` ``, `\n`. No HTML, no links — everything renders injection-safe.
- **Image** `src` is relative to your `views/<id>/` folder (ship one even if it only holds assets); no `..`, absolute paths, or URL schemes.
- **Actions** fire a bridge **request** whose name must be namespaced `<your-id>.something`; the card refuses anything else, and anything whose leading segment is a framework namespace (`ui`, `menu`, `hud`, `settings`, `views`, `game`, `runtime`). Register it with **`RegisterRequest`**, not `RegisterCommand` ([native-plugin-api.md](native-plugin-api.md)) — a button needs an outcome. An older ABI plugin may still serve the name with `RegisterCommand`, but its compatibility auto-ack is a silent success that can't express a result or failure. Actions therefore need a native plugin; Papyrus has no equivalent registration.

  The card sends `{ "mod": "<your-id>", "key": "<the action's key>" }` and waits 5 s. Resolve `{}` for a silent success or `{ "message": "…" }` to raise a toast; reject with your own code and message and that message toasts as a failure; time out and the player reads `No response from <your mod>`.

---

## 5. Presets

Author-shipped value sets, applied as a batch of ordinary validated writes. Partial maps are fine — unlisted keys are untouched:

```jsonc
"presets": [
  { "id": "performance", "label": "Performance", "description": "Lightweight HUD",
    "values": { "hud.mode": "compact", "hud.opacity": 0.6 } },
  { "id": "cinematic", "label": "Cinematic",
    "values": { "hud.mode": "full", "hud.opacity": 1.0 } }
]
```

## 6. Branding the card

Top-level, both optional:

- `"accent": "#7a9a5e"` — tints your detail pane.
- `"icon": "badge.svg"` — path inside `views/<id>/`, shown in the rail and launcher cards instead of the initials monogram. SVG or PNG, drawn at ~30–52 px square.

---

## 7. Hotkeys

Every `"type": "key"` setting is a **live, rebindable hotkey** — you never write input-hook code:

```jsonc
{ "key": "toggleHud", "label": "Toggle HUD", "type": "key", "default": "F8" }
```

- The user rebinds by pressing the button in your card. Capture happens in the native input layer, so even the overlay toggle key itself is rebindable.
- When the bound key is pressed in-game, OSF UI dispatches it to you — [§8](#8-using-your-settings-consumption) covers web and C++ delivery. Dispatch happens **only during gameplay**: suppressed while any game menu is open (pause, inventory, dialogue, main menu…) and while the overlay captures input, so you never double-handle typing or react to in-menu presses.
- Conflicts with other mods or Starfield's own bindings are **informational warnings** — never blocked, both mods still fire.
- `"allowUnbound": true` permits `""` as a deliberate unbound state (adds an unbind × in the UI; unbound keys never dispatch and never warn).

If your mod suppresses gameplay controls during a modal state (a scene, a minigame), declare an input context so intentional reuse of gameplay keys doesn't warn:

```jsonc
"inputContexts": [
  { "id": "scene", "label": "During scenes", "blocksGameplay": true }
],
"groups": [{ "settings": [
  { "key": "progressScene", "type": "key", "default": "Space", "inputContext": "scene" }
] }]
```

`blocksGameplay` is an author assertion — only use it when the game's bindings genuinely cannot fire while your context is active. It suppresses `@game` warnings only; mod-to-mod collisions still warn. Context ids are local to the mod, must match `[A-Za-z0-9][A-Za-z0-9._-]{0,63}`, and can't be `gameplay`. Missing, invalid, duplicate or unknown definitions fall back to the implicit Gameplay context; for duplicate ids the first valid definition wins.

### Start Papyrus lazily from a hotkey

A key may name an immutable GLOBAL Papyrus callback. OSF UI queues it after the normal web, C ABI and registered-Papyrus hotkey channels, so a mod can start its gameplay quest on demand without keeping a bootstrap quest running:

```jsonc
{
  "key": "startScene",
  "label": "Start scene",
  "type": "key",
  "default": "F8",
  "onPress": {
    "script": "MyMod_Hotkeys",      // script name, without .pex
    "function": "OnHotkey"
  }
}
```

The target must be a GLOBAL function with exactly two string parameters:

```papyrus
ScriptName MyMod_Hotkeys Hidden

Function OnHotkey(string asModId, string asKey) Global
    Quest target = Game.GetFormFromFile(0x000800, "MyMod.esm") as Quest
    If target != None
        target.Start()
    EndIf
EndFunction
```

The number is the record's plugin-local FormID, not its load-order-dependent runtime FormID; OSF UI never resolves or stores the quest identity itself. `onPress` is read-only schema metadata — never copied into the user's values file, and no settings write can change it.

The gameplay/menu/rebind suppression rules still apply, and the key is still delivered to ordinary subscribers, so a script that also registers the same callback gets a second delivery. Malformed or unavailable targets leave the ordinary hotkey working and appear in System Health with author details. Older OSF UI builds ignore `onPress`, so declare the `targetVersion` of the release where it ships.

For an installable notification-only test, see [`examples/declarative-hotkey-papyrus/`](../examples/declarative-hotkey-papyrus/) — it compiles and deploys without an `.esp` and exercises first press, rebinding, save-load persistence, menu suppression and failure diagnostics.

---

## 8. Using your settings (consumption)

The schema stores values; making them *do* something is your half. Pick the surface where your logic lives.

### From your own web view (zero native code)

The registry is a **state key**: subscribing replays the current value immediately, and again on every document your view loads. Individual commits arrive as **events**.

```js
// What is TRUE NOW. Fires synchronously on subscribe, and on every reload.
osfui.state.on("osfui/settings", (data) => {
  const mine = data.mods.find((m) => m.id === "yourname.mymod");
  if (mine) applyAll(mine.values);          // { "hud.opacity": 0.8, ... }
});

// What just HAPPENED: one committed value.
osfui.on("settings.changed", (p) => {
  if (p.mod === "yourname.mymod") applySetting(p.key, p.value);
});

// A hotkey press is a happening too — see §7.
osfui.on("ui.hotkey", (p) => {
  if (p.mod === "yourname.mymod" && p.key === "toggleHud") toggle();
});
```

No initial read to issue, nothing to re-request after an F5.

Values arrive post-validation — clamped and canonicalized by the same native path the settings menu writes through. The whole registry is re-sent only when its *shape* changes (a mod loads, a schema registers at runtime, a reset lands); ordinary value commits ride the event. Wire up **both**: the state key hands you the truth at every boot, the event keeps it true afterwards. Full protocol reference: [authoring-views.md](authoring-views.md), [`sdk/osfui.d.ts`](../sdk/osfui.d.ts).

### Writing settings from a view

Writes are **requests** — they can fail, so they settle:

```js
try {
  // Resolves the COMMITTED value, post-clamp: you can tell "clamped" from
  // "accepted" without a re-fetch.
  const { value } = await osfui.request("settings.set", {
    mod: "yourname.mymod", key: "hud.opacity", value: 1.4,
  });
  // value === 1 when your schema caps max at 1
} catch (err) {
  // err.code: "forbidden" | "unknown-setting" | "read-only" | "invalid-value"
}

// One key, or the whole mod when `key` is omitted. Resolves {} — the refreshed
// registry reaches every view (including yours) as `osfui/settings` state.
await osfui.request("settings.reset", { mod: "yourname.mymod" });
```

A view may only write **its own** mod (the built-in Mods surface and keybinds board are the two exceptions), so a neighbour can't rewrite your settings — or OSF UI's overlay toggle key, the player's guaranteed way out. Anything else rejects `forbidden`.

Rebinding a key is the one flow that waits on a human, and is split accordingly: the request settles in **machine** time, the human-time outcome arrives as an event.

```js
osfui.on("settings.captured", (p) => {
  if (p.cancelled) return;                  // Escape, or an unbindable key
  osfui.request("settings.set", { mod: p.mod, key: p.key, value: p.name });
  // p.conflicts (if present) lists binds this WOULD collide with — warn, never block
});

// Resolves { armed: true, mod, key } as soon as capture is armed; rejects
// "capture-busy" | "forbidden" | "not-rebindable".
await osfui.request("settings.captureKey", { mod: "yourname.mymod", key: "toggleHud" });
```

Nothing here needs a disabled client timeout: a request pending until the player presses a key can't be told apart from a backend that died.

### From an SFSE plugin (C++)

Fetch the bridge from [`sdk/OSFUI_API.h`](../sdk/OSFUI_API.h) through the `Client` wrapper (C ABI 1.8; the calls below predate 1.8 and remain compatible — see [native-plugin-api.md](native-plugin-api.md)):

```cpp
static OSFUI::API::Client g_ui;   // g_ui.Init() once, after SFSE kPostLoad

// Typed getters — synchronous, callable from any thread.
bool enabled = false;
g_ui.GetSettingBool("yourname.mymod", "enabled", &enabled);

// Change subscription — fires on the game main thread, and REPLAYS once per
// current value on subscribe, so you need no separate initial read.
g_ui.SubscribeSettings("yourname.mymod",
    [](const char* mod, const char* key, const char* valueJson, void* user) noexcept {
        // switch on key; valueJson is the JSON-encoded value
    }, nullptr);

// Hotkeys — fires on the game main thread when the bound key is pressed.
g_ui.SubscribeHotkey("yourname.mymod", "toggleHud",
    [](const char* mod, const char* key, void* user) noexcept { /* toggle */ }, nullptr);
```

A plugin built against ABI 1.0–1.7 receives the 1.8 bridge normally; older vtable slots and feature numbers are unchanged. Recompile only to use the new retained-state method or newer header conveniences.

A DLL can skip the drop-in file and register at runtime with `RegisterSettingsSchema(json)` — same JSON, same values file, so a mod can move between the two without users losing settings. If both exist, the DLL registration wins (with a logged warning).

### From Papyrus

An esm+scripts mod needs **no DLL and no registration call**: the drop-in schema file *is* the registration, and the shipped `OSFUI` script (`Data/Scripts/OSFUI.pex`, source + full API docs in `Data/Scripts/Source/OSFUI.psc`) reads it back:

```papyrus
; Feature-detect: 0 => OSF UI absent (natives unbound; every call then yields the
; default you pass). Packed major*10000 + minor*100 + patch.
If OSFUI.GetVersion() >= 10000   ; needs 1.0.0+
    Float scale = OSFUI.GetFloat("yourname.mymod", "hud.scale", 1.0)
EndIf
```

- **Getters** — `GetBool` / `GetInt` / `GetFloat` / `GetString(modId, key, default)`: cheap, thread-safe reads of the live value store. Unknown mod/key or a type mismatch yields the default. `GetString` covers string-, enum- and key-typed settings. Ids, keys and enum option values match case-insensitively (Papyrus string interning can't preserve casing); write them as authored anyway.
- **Setters** — `SetBool` / `SetInt` / `SetFloat` / `SetString(modId, key, value)` and `Reset(modId, key = "")`: fire-and-forget; validated/clamped against your schema and persisted through the same path as the settings menu (refusals logged to `OSF UI.log`). An open settings card updates live.
- **Change events + hotkeys** — register a callback; hotkeys are just your `"type": "key"` settings (§7), so the user sees and rebinds them while OSF UI owns the input hook:

```papyrus
ScriptName MyModQuest Extends Quest

Function RegisterAll()
    OSFUI.RegisterForSettingChanges(self as ScriptObject, "OnSettingChanged", "yourname.mymod")
    OSFUI.RegisterForHotkey(self as ScriptObject, "OnHotkey", "yourname.mymod", "toggleHud")
EndFunction

Function OnSettingChanged(string asModId, string asKey)
    ; fires after ANY writer commits (menu, native, Papyrus) — re-read via getters
EndFunction

Function OnHotkey(string asModId, string asKey)
    ; gameplay-only delivery: never fires while the user types in an overlay
    ; or rebinds a key, and never consumes the press
EndFunction
```

Registrations are **session-scoped** (they don't survive a save load) — call `RegisterAll()` from quest init *and* every game load (e.g. `OnPlayerLoadGame` on a player `ReferenceAlias`). `RegisterFor*` returns a token for `Unregister(token)`; `...Static` variants dispatch to global functions on a named script. `OpenMenu()` opens the Mods surface (same as F10); view ids are qualified, so it defaults to `"osfui/settings"`.

Settings cover pre-declared scalars. For **dynamic data** — pushing live lists/tables to your own view and reacting to its clicks, all from Papyrus — see [authoring-dynamic-data.md](authoring-dynamic-data.md).

---

## 9. Updating your mod

- **Declare `"version": 1`** from day one (a plain integer you bump on meaningful schema changes; stamped into values files as `$schemaVersion` for diagnostics). Never name a setting key with a leading `$` — those are reserved host meta keys.
- **Renaming a key:** keep the old name as an alias; saved values migrate on the next load, no version arithmetic:

  ```jsonc
  { "key": "hud.opacity", "aliases": ["opacity"], "type": "float", ... }
  ```

- **Changing a default:** just change it. Persistence is sparse (only user-changed values are written), so users who never touched the knob get the new default.
- **Changing a type:** old saved values that no longer validate fall back to the new default. Prefer a new key with an alias when the meaning changes.
- **Using features newer than the OSF UI you tested on:** declare `"targetVersion": "2.0.0"`. The schema still loads best-effort on older hosts — unknown decorations ignored, unknown types rendered read-only (a write to one rejects `read-only`, not `invalid-value`, so a view can say "needs a newer OSF UI"), saved values for unknown types preserved untouched — and the Mods surface badges "needs update" naming your mod. A schema target remains advisory; a **view manifest** below 2.0 selects the 1.x compatibility helper so existing view code can continue running.
- **Uninstall:** the values file is deliberately kept (MO2 profile switches look identical to uninstalls). Reinstalling restores the user's settings.

---

## 10. Testing your schema

**Browser harness — no game launch.** `npm --prefix frontend run dev` (see [`frontend/README.md`](../frontend/README.md)), open `http://localhost:8080/?view=osfui/settings`, drag your JSON onto the page (or pass `?schema=<url>`). It renders the *real* settings view with a mock bridge that mirrors native clamping, persists to localStorage, and logs the exact bridge traffic. Widgets, conditions, presets, actions and rebinding all work.

**Editor validation.** The `$schema` line catches most mistakes as you type. For CI:

```
npx ajv-cli validate --spec=draft2020 -s docs/schema/settings-schema.schema.json -d yourname.mymod.json
```

**In-game hot reload.** With `"devMode": true` in OSF UI's `config.json`, saved changes to `settings\*.json` are picked up within ~1 s — values survive, the open menu repaints. A loaded view's own HTML/JS/CSS reloads the same way.

**Broken files are loud.** A bad filename or unparseable JSON is skipped and reported (with line/column) in an alert pinned atop the Mods rail; a corrupt values file is quarantined to `<id>.json.bad` and defaults served. If your card doesn't appear, look there first, then at `OSF UI.log`.

**When a write doesn't stick.** Every failure the bridge can attribute to you prints to the calling page's console with an `[osfui]` prefix, so F12 DevTools (harness or in-game devMode) shows the rejection code and payload. For the whole picture, `localStorage["osfui:trace"] = "1"` and reload.

---

## 11. Localization

Write plain English in your schema — no string keys, nothing extra to maintain. Translators (you, or the community, as a separate data mod) ship:

```
Data\SFSE\Plugins\OSFUI\l10n\<id>_<locale>.json     e.g. yourname.mymod_de.json
```

A flat map from structural addresses to translated text; partial files are fine, and the authored English is the fallback for every missing entry:

```json
{
  "settings.title": "Mein Mod",
  "settings.hud.enabled.label": "HUD aktivieren",
  "settings.hud.mode.options.compact": "Kompakt",
  "groups.general.label": "Allgemein"
}
```

Addresses derive from your stable identities — setting keys, stored option values, and the optional `id` on groups/presets/notes/images (give those an `id` so translations survive reordering; the array index is the fallback). Mod ids, setting keys, stored `options` and commands are never localized. Worked pair: [`examples/settings-only/l10n/`](../examples/settings-only/l10n/).

---

## 12. Reference

- **Formal schema (autocomplete + validation):** [`docs/schema/settings-schema.schema.json`](schema/settings-schema.schema.json)
- **Copy-me template exercising every widget:** [`examples/settings-only/`](../examples/settings-only/)
- **Bridge protocol:** [authoring-views.md](authoring-views.md), [`sdk/osfui.d.ts`](../sdk/osfui.d.ts)
- **C ABI for SFSE plugins:** [native-plugin-api.md](native-plugin-api.md), [`sdk/OSFUI_API.h`](../sdk/OSFUI_API.h)
