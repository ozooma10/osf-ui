# Authoring Views & Settings

How to build a UI for OSF UI without touching the C++ runtime. This is
a reference for the two data-driven extension points that work today:

1. **Views** — an HTML/CSS/JS page rendered as an in-game overlay.
2. **Settings schemas** — typed knobs that appear in the built-in `settings`
   view (MCM-style), persisted and validated natively.

> **Status / scope.** What you can ship as pure content with no recompile: a
> `views/<modId>/<viewName>/` folder and a `settings/<modId>.json` schema. The
> bridge protocol is at version **1.0 — stable**; additive changes bump the
> minor and surface as new capabilities, breaking changes bump the major.
> Feature-detect via the `capabilities` array of the `runtime.ready`
> handshake (`osfui.has()`, §3) — `bridgeVersion` is informational. See
> [ROADMAP.md](ROADMAP.md) for what is still planned.

## 0. Ids: one grammar for everything

Every public identifier derives from your **mod id** (api-freeze-plan item 1):

- **Mod id** — `<author>.<modname>`, e.g. `ozooma10.almanac`. Lowercase
  `[a-z0-9-]` segments, **exactly one dot**, max 64 chars. `author` is a
  handle you already own (Nexus/GitHub username) — self-allocated, no
  registry. Dotless ids are **reserved for the platform** (`osfui` is the
  only dotless built-in), so no reserved-word list exists to collide with.
- **View name** — `[a-z0-9-]+`, local to your mod (`planets`).
- **Qualified view id** — `<modId>/<viewName>` (`ozooma10.almanac/planets`).
  This is the id everywhere views are referenced: `config.json` `view`/`views`,
  `menu.open`, `views.data`, `RegisterView`. The slash mirrors the folder path
  (a dotted join would be ambiguous, since mod ids contain a dot).

Ids failing the grammar are **hard-rejected at load** with an ERROR in
`OSF UI.log` naming the file and the rule. The same mod id names your settings
schema (`settings/<modId>.json`), your values file, your view namespace
folder, and the prefix of your native bridge commands
(`<modId>.<command>`) — one identity across every surface.

**Claimed author prefixes.** The author segment is self-allocated (use a
handle you already own — your Nexus/GitHub name), with two claims on record:
`osfui` is the platform's (reserved, dotless) and **`osf`** is the OSF family
of mods (`osf.animation`, and future `osf.*` siblings). Don't publish under
an author segment someone else is already shipping under — the collision
policy is deterministic first-wins, and the loser is you.

Everything here assumes you have read [security-model.md](security-model.md):
**your view is treated as untrusted code.** There is no network, no filesystem
beyond your own folder, and no way to call arbitrary native functions.

---

## 1. View package layout

A view is a folder **inside your mod's namespace folder** under the plugin
data dir:

```
SFSE/Plugins/OSFUI/views/<author>.<modname>/<viewname>/
  manifest.json     required — declares the view
  index.html        your entry page (name configurable via manifest "entry")
  style.css         (optional) your styles
  main.js           (optional) your logic
  assets/...        (optional) images/fonts — local only
```

The two-level layout is discovered automatically at load (a mod folder may
hold several views; subfolders without a `manifest.json` are ignored, so you
can keep shared assets next to your views). The built-ins dogfood it:
`views/osfui/settings/`, `views/osfui/keybinds/`. To open your view, use its
qualified id `<modId>/<viewName>` — e.g. as a `config.json` `views` entry, a
`menu.open` target, or a `RegisterView` argument.

All asset references must be **relative and stay inside your folder**.
Absolute paths, paths with a root, and any `..` component are rejected before
disk I/O (`SandboxFileSystem`, enforced in the renderer). The page is loaded
as `file:///<modId>/<viewName>/<entry>`. The sanctioned exceptions: the shared
UI kit at `views/shared/osfui.css` / `osfui.js` — link them as
`../../shared/osfui.css` (they collapse to `file:///shared/…`).

### The shared UI kit — contract

`shared/osfui.css` is the design system every shipped view uses. Its contract
(api-freeze item 9):

- **Everything the kit exports is prefixed**: classes `osf-*`, custom
  properties `--osf-*`. Nothing un-prefixed is contract — your own class and
  token names can never collide with a kit update.
- **Linking is the opt-in, and it is all-or-nothing.** The sheet styles
  element-level bases (body, headings, `a`, `kbd`, `::selection`, scrollbars,
  form elements) globally — that is what a design-system base sheet is. Link
  it for the native look, or don't link it and own all your styling; there is
  no partial mode.
- **Theming is one hex.** There are no theme classes; a mod's accent is its
  schema/manifest `accent` value. Apply it to any subtree with
  `osfui.applyAccent(el, "#e6904a")` (from `shared/osfui.js`) — it derives
  the kit's linked accent set (`--osf-accent`, `-hover`, `-strong`,
  `-quiet`); passing a missing/invalid value clears it.

---

## 2. `manifest.json` reference

```jsonc
{
  "id": "myhud",            // REQUIRED; MUST equal the view folder name. The runtime id is the qualified "<modId>/myhud", derived from the path
  "title": "My HUD",        // optional, defaults to the qualified id
  "description": "",        // optional; one-line blurb shown in catalogs (views.data / the Mods surface)
  "hub": true,              // optional, default true; false = hidden utility view — loads and works, but isn't advertised in catalogs (name predates the Mods surface)
  "targetVersion": "1.0.0", // optional; the OSF UI version this view is authored against — advisory, never gates loading (see note below)
  "entry": "index.html",    // optional, default "index.html"; must stay inside the folder
  "width": 1280,            // optional, default 1280; clamped to 1..16384 — logical (authoring) size
  "height": 720,            // optional, default 720;  clamped to 1..16384 — logical (authoring) size
  "transparent": true,      // optional, default true; lets the game show through
  "zorder": 0,              // optional, default 0; compositing layer when several views are hosted — higher draws on top
  "interactive": true,      // optional, default true; false = never receives input/focus (e.g. a passive HUD)
  "permissions": {          // optional; everything defaults to DENY
    "nativeBridge": true,   // false ⇒ no window.osfui bridge is created at all
    "filesystem": false,    // reserved; no effect yet
    "network": false         // reserved; forced off with a warning if set true
  }
}
```

Notes:
- **Unknown manifest keys are fine** (a manifest written for a newer OSF UI
  parses leniently; devMode logs them at INFO). An optional
  `"manifestVersion"` integer is accepted but not required — the nested
  folder layout is itself the format discriminator (item 8).
- **`targetVersion`** declares the OSF UI version your view was authored
  against (`"<major>[.<minor>[.<patch>]]"`, e.g. `"1.1.0"`). It is advisory:
  the view always loads and does what it can. When the running OSF UI is
  older than the target, a warning lands in `OSF UI.log` and the Mods surface
  shows a **"needs update"** badge next to the OSF UI version number, with
  your mod named in the tooltip — so the user learns *OSF UI* is what needs
  updating, instead of blaming your mod for missing features. A malformed
  value is ignored with a warning. Feature-gate at runtime with
  `osfui.has(...)` regardless — `targetVersion` informs the user, not your
  code.
- **`width`/`height` are the page's LOGICAL size — author against it.** When the
  `d3d12` compositor is active, the runtime resizes the view to match the screen
  aspect (height-capped at 1440) **with a matching device scale**
  (`outputHeight / height`), so the page always lays out at its logical height
  and CSS px scale up to output pixels. In practice: at 1440p a 720-tall
  manifest gets a 2.0 device scale and a CSS viewport 720 tall — type sized for
  720p stays that size on screen instead of shrinking. Width still varies with
  the screen's aspect ratio (e.g. ~1720 CSS px wide on a 21:9 display), so
  author width-responsive CSS. **The layout guarantee — versioned (protocol
  1.0): your manifest's logical HEIGHT is the fixed contract; width is not.**
- **`permissions.nativeBridge` must be `true`** if your page talks to the
  runtime. With it `false`, `window.osfui` is never injected and your page
  runs purely client-side.
- A manifest that fails validation (`id` ≠ folder name, escaping `entry`, a
  folder name violating the id grammar) is skipped with an error in
  `OSF UI.log`. The owning mod id is taken from the `views/<modId>/` folder — a
  view always groups onto its own mod's page automatically.

### Multiple views & layering

Several views can be hosted and composited at once. `config.json` lists them
by qualified id:

```jsonc
{
  "view": "osfui/settings",                          // the ACTIVE view: receives input + the bridge
  "views": ["osfui/settings", "yourname.mymod/hud"]  // the set of views to load (membership only)
}
```

> **Shipping a view with a native mod?** Don't edit the user's `config.json` —
> your SFSE plugin can register its shipped `views/<modId>/<viewName>/` folder
> at runtime with one bridge call (`RegisterView("<modId>/<viewName>")`, C ABI
> 1.5). The view then joins the views
> catalog — it appears on the Mods surface as a launch card on the Home page
> and on its mod's page (grouped automatically by the `views/<modId>/` folder it
> lives under), and opens via `RequestMenu` / `menu.open`. See
> [native-plugin-api.md](native-plugin-api.md) §5c. The `views` array is for the
> user's own composition (and OSF UI's built-ins).

- **Layering** is by each view's manifest `zorder` (not the array order): lower
  draws beneath, higher on top; ties keep load order. A HUD with `zorder: 100`
  always sits above a `zorder: 0` menu.
- **The focus model — a versioned guarantee (protocol 1.0).** Input goes to
  exactly one view: the **top open menu** (the single-menu stack — opening a
  menu replaces and focuses it). A view with `interactive: false` (a passive
  HUD) is never focused and never receives input, even when it is the top
  layer. There is no user-facing focus-cycle key.
- **Each bridge-enabled view (`nativeBridge: true`) has its own bridge.**
  Messages are attributed to their source view and replies route back to it, so
  several views can talk to native independently — even a passive HUD can
  receive native pushes and post messages.
- Each view is sized to the whole screen, so position your content with CSS and
  keep the rest transparent; the layers blend by alpha.

### Mouse & cursor

**Do not draw your own pointer.** While the overlay captures input the runtime
shows the real Windows (hardware) cursor — zero-lag, composited by the display
hardware, independent of game framerate. Your page's CSS `cursor` property maps
to the matching system cursor (`pointer` → hand, `text` → I-beam, resize
variants, `none` hides it, anything exotic falls back to the arrow), so hover
feedback works exactly like a browser. A page-drawn `<div>` pointer would
trail the real one by the full render pipeline (the shipped views used to do
this — it felt laggy and was removed) and `cursor: none` on `body` would hide
the pointer for the whole view.

---

## 3. The bridge — `window.osfui`

When `nativeBridge` is granted, the runtime injects one object before your page
scripts run. **Use it through the shipped helper** — load it like the shared
stylesheet, before your own script:

```html
<script src="../../shared/osfui.js"></script>
<script src="main.js"></script>
```

```js
// The helper's whole surface (thin by design — it is part of the contract):
osfui.available()                 // bridge present? false = plain browser
const info = await osfui.ready;   // the runtime.ready payload
osfui.has("type:flags")           // capability test (see Versioning, §7)
osfui.send("close");              // fire-and-forget ui.command
const reply = await osfui.request("settings.get");   // correlated request
const off = osfui.on("settings.changed", (payload) => { ... });  // subscribe
```

`osfui.request()` generates a `requestId`, and resolves with the reply message
(`{ type, requestId, payload }`) or rejects — an `Error` with a stable `.code`
— on `ui.error`, on `ui.result { ok:false }`, on timeout (default 10 s; pass
`{ timeoutMs: 0 }` to disable, e.g. for a key capture that waits on the user),
and immediately when no bridge is present. With the helper loaded it owns
`osfui.onMessage` — subscribe with `osfui.on()`, never assign `onMessage`
yourself.

Under the helper sit two primitives (all the helper itself uses):

```js
// web → native: send one JSON message
window.osfui.postMessage(jsonString);

// native → web: the runtime calls it with a JSON string (owned by the helper)
window.osfui.onMessage = (jsonString) => { ... };
```

`postMessage` is read-only and cannot be reassigned. Messages sent before
`onMessage` exists are queued (bounded, FIFO) and flushed once the DOM is
ready **and** `onMessage` is installed — loading `osfui.js` in `<head>` or
before your script satisfies this. A view that has queued messages it cannot
yet receive is kept off screen until they are delivered (the plugin-API "open
a view in a specific state" guarantee, ABI 1.3 — see
`docs/native-plugin-api.md` §6a), and a page that never installs `onMessage`
while being sent messages stays hidden with a warning in the SFSE log.

### Message envelope

Every message in both directions is JSON text with this shape:

```jsonc
{ "type": "<string>", "requestId": "<optional string>", "payload": { ... } }
```

`requestId` (protocol 1.0) is the correlation contract — `osfui.request()`
does this for you: any `ui.command` may carry a caller-chosen id (string,
1–64 chars); **every reply echoes it top-level**, and a command with no reply
type of its own (`close`, `menu.open`, …) answers
`ui.result { ok, command, code?, message? }` — but **only when an id was
supplied**. Omitting it is fire-and-forget.

Malformed messages are rejected and logged, never fatal. Both directions are
capped at 64 queued messages (excess is dropped with a warning) — **do not
flood the bridge**; it shares the game thread.

### Web → native

There is exactly **one** accepted inbound type: `ui.command`. The `payload`
carries a `command` field plus that command's arguments — `osfui.send(command,
fields)` / `osfui.request(command, fields)` build it for you.

Whitelisted commands (anything else is rejected + logged, and answered with
`ui.error { code: "unknown-command" }`):

| command | payload fields | effect |
|---|---|---|
| `close` | — | close the calling surface (closing the last open menu hides the overlay; a coexisting live HUD stays up) |
| `setVisible` | `visible: bool` | open/close the calling surface |
| `menu.open` | `view?: string` | open a registered surface by id (omitted ⇒ the calling view). Opening a menu also **focuses** it (single-menu policy: it replaces the current menu, so the newly opened menu is the input target) |
| `menu.close` | `view?: string` | close a registered surface by id (omitted ⇒ the calling view) |
| `hud.show` / `hud.hide` | `view?: string` | aliases of `menu.open` / `menu.close` — a surface's kind (menu vs. HUD) is fixed by its manifest, not by which command you use |
| `setViewHidden` | `view?: string`, `hidden: bool` | show/hide one *loaded* view, independent of the global overlay toggle (omitted `view` ⇒ self) |
| `log` | `text: string` | write to `OSF UI.log` (truncated to 512 chars) |
| `ping` | — | runtime replies with `runtime.pong` |
| `game.get` | — | runtime replies with `game.data` (in-game date/time from the calendar) |
| `views.get` | — | runtime replies with `views.data` (catalog of loaded surfaces) **and subscribes the caller**: any later open/close/focus/load-state change pushes a fresh `views.data` unsolicited — no polling needed |
| `settings.get` | — | runtime replies with `settings.data` |
| `settings.set` | `mod, key, value` | set one schema-declared setting (validated) |
| `settings.reset` | `mod`, `key?` | reset one key, or the whole mod if `key` omitted |
| `settings.captureKey` | `mod, key` | arm native key-rebind capture for ANY `key`-typed setting of any mod; the next key press replies with `settings.captured` — echoing the arming `requestId`, however many ticks later, so `osfui.request("settings.captureKey", …, {timeoutMs: 0})` awaits the whole rebind. One capture at a time: a second arm answers `ui.result { ok:false, code:"capture-busy" }`. Captured natively so pressing the current toggle key rebinds instead of closing the overlay |
| `osfui.gamepadRaw` | `raw: bool` | *(experimental — gate on the `gamepad` capability; exempt from the 1.0 stability guarantee until stabilized)* take over gamepad handling: suppress the default nav mapping and consume raw `ui.gamepad` events yourself. **Sticky per view** — the grant survives overlay hide/show and clears only when your page (re)loads or the view is destroyed; other views never inherit it |

> There is intentionally **no** "call any native function" escape hatch. New
> commands come from native code only: either a handler in the OSF UI runtime,
> or a **separate SFSE plugin** registering its own commands through the native
> bridge API ([native-plugin-api.md](native-plugin-api.md)). Plugin commands
> must be shaped `<author>.<modname>.<name>` (two dots minimum — the leading
> mod id is the §0 grammar), so every platform command above is structurally
> unregisterable by mods.

### Native → web

Subscribe with `osfui.on(type, fn)` (replies that resolve an `osfui.request()`
are ALSO dispatched to subscribers, so one render path serves both):

| type | payload | when |
|---|---|---|
| `runtime.ready` | `{ game, plugin, version, bridgeVersion, capabilities }` | once, after your page loads (`osfui.ready` resolves with it) — your cue to request data. Gate features on `capabilities` (append-only names, §7); `bridgeVersion` is informational |
| `runtime.pong` | `{}` | reply to your `ping` |
| `game.data` | `{ calendar: { available, day, month, year, hour, daysPassed } }` | reply to `game.get`; each provider nests under its own object (future providers are siblings of `calendar`); `available:false` before a save is loaded |
| `views.data` | `{ views: [ { id, title, description, mod, kind, interactive, hub, open, focused, loadState } ] }` | reply to `views.get`, and re-pushed to every subscribed view when any entry changes. `id` is the qualified `<modId>/<viewName>`; `mod` = the owning mod id derived from the folder (a mod with no settings schema of that id just lists under its own title); `kind` = `"menu"`\|`"hud"`; `loadState` = `"loading"`\|`"loaded"`\|`"failed"`; a view torn down by crash-recovery drops out of the list. Respect `hub:false` (don't list those) |
| `settings.data` | `{ mods: [ { id, title, schema, values } ], vanillaKeys? }` | reply to `settings.get` / after a `settings.reset`. A `key`-typed setting whose binding collides with another mod's carries runtime-injected `conflicts: [{mod, key, title}]` in its schema object — informational; render a badge, never block. `mod` may be the reserved id `@game` (the game's own bindings; display `title`, e.g. "Starfield (Quicksave)"). Top-level `vanillaKeys: [{event, title, name}]` is the game's FULL binding table (read-only; the keybinds view renders it); absent when the runtime has none |
| `settings.ack` | `{ mod, key, ok, value?, code?, message? }` | result of a `settings.set`. `ok:true` carries `value`, the authoritative post-clamp committed value (compare with what you sent to detect clamping — no re-fetch needed); `ok:false` carries a stable `code`: `unknown-setting`, `read-only` (requires-gated stub or a type this host doesn't know), or `invalid-value` |
| `settings.captured` | `{ mod, key, name, cancelled, conflicts? }` | reply to `settings.captureKey`: the captured key `name` (an OSF UI key name), or `cancelled:true` (Escape / unbindable — keep the old binding). When the captured key is already bound elsewhere, `conflicts: [{mod, key, title}]` lists actionable collisions this bind would create; expected `@game` reuse from a `blocksGameplay` context is omitted. Warn live, never block. The view then sends a normal `settings.set` with `name` |
| `ui.hotkey` | `{ mod, key }` | the physical key currently bound to that `key`-typed setting was pressed in-game (protocol 1.0). Pushed to every `settings.get` subscriber — filter on `mod`. Suppressed while the overlay captures input or a rebind is armed; rebinds re-route automatically |
| `ui.result` | `{ ok, command?, code?, message? }` | the uniform outcome (protocol 1.0), sent ONLY when your `ui.command` carried a `requestId`: verb commands (`close`, `menu.open`, …) answer `ok:true` on success; failures carry a stable `code` (`unknown-view`, `capture-busy`, `unknown-setting`, …). A plugin-registered command acks `ok:true` = delivered to the plugin's handler. `osfui.request()` consumes this for you |
| `ui.gamepad` | `{ kind:"button", button:{id, down} }` \| `{ kind:"stick", axes:{lx, ly, rx, ry} }` | *(experimental — gamepad navigation is being refined; gate on the `gamepad` capability; exempt from the 1.0 stability guarantee until stabilized)* raw gamepad events to the ACTIVE view while the overlay captures input (per-kind nesting, protocol 1.0). The default nav mapping (D-pad→arrows, A→Enter, B→close, sticks→scroll) also applies unless you asserted `osfui.gamepadRaw` |
| `ui.visibility` | `{ visible }` | the receiving view was shown/hidden with the overlay (edge-triggered). The reference views scope per-visit state off this |
| `ui.error` | `{ code, message, type?, command? }` | the runtime rejected something you sent. `code` is a stable machine string — `malformed-message`, `unknown-message-type`, `unknown-command`; `message` is the human sentence. Echoes your `requestId` when it could be read, so `osfui.request()` rejects with it. The same WARN is in `OSF UI.log` |

Unknown `type`s should be ignored (never `eval`'d) — including future `type`s
this runtime version doesn't know about.

### Minimal example

```html
<script src="../../shared/osfui.js"></script>
<script>
"use strict";
osfui.ready.then((info) => {
  document.title = `Connected to ${info.plugin} v${info.version}`;
  if (osfui.has("settings")) osfui.send("settings.get");  // read + subscribe
});
osfui.on("settings.changed", (p) => {
  if (p.mod === "yourname.mymod") applySetting(p.key, p.value);
});
document.getElementById("close").onclick = () => osfui.send("close");

// When the outcome matters, request() instead of send():
async function openAlmanac() {
  try { await osfui.request("menu.open", { view: "yourname.mymod/almanac" }); }
  catch (err) { console.warn("open failed:", err.code); }  // e.g. "unknown-view"
}
</script>
```

See [`data/OSFUI/views/osfui/settings/main.js`](../data/OSFUI/views/osfui/settings/main.js)
for a complete, commented example.

---

## 4. Settings schemas (the MCM-style path)

Drop a JSON schema at:

```
SFSE/Plugins/OSFUI/settings/<author>.<modname>.json
```

Every schema in that folder is loaded as a separate "mod" and rendered as its
own card in the built-in `settings` view — **with zero per-mod native or web
code**. Values persist per-mod to a user-writable path
(`Documents\My Games\Starfield\OSFUI\settings\<id>.json`), survive
relaunch, and the runtime can react to changes natively.

### Schema format

```jsonc
{
  "id": "yourname.mymod",        // optional, defaults to the filename stem; "<author>.<modname>" grammar (see §0)
  "title": "My Mod",             // shown as the card header
  "groups": [
    {
      "label": "Gameplay",       // optional section heading
      "settings": [
        { "key": "enabled",  "label": "Enabled",      "type": "bool",  "default": true },
        { "key": "count",    "label": "Item count",   "type": "int",   "min": 0, "max": 99, "step": 1, "default": 10 },
        { "key": "opacity",  "label": "HUD opacity",  "type": "float", "min": 0.1, "max": 1.0, "step": 0.05, "default": 0.8 },
        { "key": "mode",     "label": "Mode",         "type": "enum",  "options": ["off", "compact", "full"], "default": "compact" },
        { "key": "label",    "label": "Custom label", "type": "string", "default": "Hello" }
      ]
    }
  ]
}
```

### Input contexts for intentional key reuse

A mod that disables Starfield gameplay controls during a modal state can
declare a named context once and reference it from each scene-only key:

```json
{
  "inputContexts": [
    { "id": "scene", "label": "During OSF scenes", "blocksGameplay": true }
  ],
  "groups": [{ "settings": [
    { "key": "progressScene", "type": "key", "default": "Space", "inputContext": "scene" }
  ] }]
}
```

`blocksGameplay` is an author assertion: use it only when the mod really
prevents the curated Starfield gameplay bindings from firing. It removes
`@game` warnings for that key, but collisions with other mods still warn and
all duplicate mod bindings still dispatch. Context ids are local to the mod,
must match `[A-Za-z0-9][A-Za-z0-9._-]{0,63}`, and cannot be `gameplay`.
Missing, invalid, duplicate, or unknown definitions fall back conservatively
to the implicit Gameplay context; for duplicate ids, the first valid
definition wins.

### Type rules (enforced natively in `SettingsStore`)

| type | control | validation on write |
|---|---|---|
| `bool` | checkbox | must be a boolean |
| `int` | slider | number, clamped to `[min,max]`, rounded |
| `float` | slider | number, clamped to `[min,max]` |
| `enum` | dropdown | must be one of `options` |
| `flags` | checkbox group | array of `options` strings — multi-select. Unknown options and duplicates are filtered out (the enum-removal analogue of numeric clamping) and the stored array is canonicalized to declared-option order |
| `string` | text field | truncated to 256 chars |
| `key` | press-to-bind button | key-name string (≤16 chars), non-empty unless the setting sets `"allowUnbound": true` — then `""` is the deliberate unbound state (no hotkey dispatch, no conflict badges, and the UI adds an unbind ×). **Framework-managed:** capture is armed via `settings.captureKey` and grabbed in the native input layer (so pressing the current toggle key rebinds instead of closing the overlay). Every `key`-typed setting of every mod is rebindable and dispatches via the HotkeyService (`ui.hotkey` / `SubscribeHotkey`) |

This is the **frozen base type set** (api-freeze-plan item 2). There is no
`color` type — use `type:"string"` + `widget:"color"`. Post-1.0 evolution is a
base type plus a `widget` hint and attributes, never a new base type outside a
capability gate.

**Forward compatibility.** A host that predates one of your setting types
renders that row read-only ("needs a newer OSF UI"), serves the schema
`default` to consumers, and **preserves the user's saved value untouched** —
it round-trips through every rewrite and the newer host picks it back up. If
your schema is unusable without a capability, gate the whole mod instead:

```jsonc
{ "id": "yourname.mymod", "requires": ["type:flags"] }
```

A host that can't satisfy every `requires` entry registers your mod as an
inert stub card (values file untouched, nothing served) instead of rendering
it half-broken. The names share one vocabulary with `runtime.ready`'s
capability list. Hosts older than the `requires` field ignore it — declare it
from your first release.

Values files carry two reserved `$`-prefixed meta keys the host owns:
`$schemaVersion` (your schema's `version`, stamped on write) and
`$formatVersion` (the sparse encoding's own version, item 8). Never name a
setting with a leading `$`; unknown `$`-keys from newer hosts round-trip like
any preserved value.

The `bool` control renders as a toggle switch, `int`/`float` as sliders with a
value badge — see `views/shared/osfui.css` for the shared control styles.

Unknown keys, wrong types, and out-of-range values are rejected/clamped
server-side and reported via `settings.ack {ok:false}`. **Untrusted JS cannot
write arbitrary keys, out-of-range values, or to any path but its own settings
file.**

### Reacting to changes from your view (zero native code)

`settings.get` doesn't just read — it **subscribes** the calling view (the
`views.get` pattern). After one `settings.get` at startup:

- every committed value — from the settings menu, a preset, a reset, or a
  native write — is pushed to you as
  `settings.changed { mod, key, value, conflicts? }` (the value is
  post-validation, i.e. authoritative; on `key`-typed settings `conflicts` is
  the recomputed collision list — protocol 1.0 — so badge updates need no
  registry re-fetch), and
- a registry *shape* change (a mod registering/unregistering a schema at
  runtime) re-sends the full `settings.data`.

So a mod's HUD view reacts live to its own settings with zero polling:

```js
osfui.on("settings.changed", (p) => {
  if (p.mod === "yourname.mymod") {
    applySetting(p.key, p.value);  // e.g. re-style, re-layout
  }
});
osfui.send("settings.get");  // initial read + subscription in one call
```

You receive changes for **all** mods — filter on `p.mod`.

### Hotkeys with zero native code (protocol 1.0)

Every `type:"key"` setting is a **live hotkey** (mcm-design.md §9): when the
user presses the bound key in-game, the runtime pushes
`ui.hotkey { mod, key }` to every `settings.get` subscriber. So the same
single subscription above also makes your HUD toggleable:

```js
osfui.on("ui.hotkey", (p) => {
  if (p.mod === "yourname.mymod" && p.key === "toggleHud") {
    osfui.send("setViewHidden", { hidden: visible = !visible });  // or hud.show/hud.hide
  }
});
```

Presses typed into an overlay text field or during a rebind capture never
fire, key repeats never fire, and the user can rebind freely — the runtime
re-resolves the binding on every change. Duplicate bindings across mods all
fire; the settings view badges them (the `conflicts` data above), but they
are never blocked.

### Reacting natively (C ABI)

Separate SFSE plugins subscribe over the native bridge — no core edit needed:
`SubscribeSettings` + typed getters for values (C ABI 1.2), `SubscribeHotkey`
for key presses (C ABI 1.4). See
[native-plugin-api.md](native-plugin-api.md) §5a/§5b and `docs/mcm-design.md`
§8.2/§9. In-tree framework knobs still react through
`Runtime::OnSettingChanged` (e.g. `osfui.toggleKey` live-rebinds the overlay's
open/close key).

---

## 5. Testing locally

Both shipped views detect a missing bridge and fall back to a **standalone
mode** so you can open `index.html` directly in a normal browser and iterate on
layout/logic without launching the game:

```js
if (!osfui.available()) {
  // running in a plain browser — stub or no-op the native calls
}
```

(`shared/osfui.js` installs itself even without a bridge, so `osfui.available()`
/ `osfui.on()` are always safe to call; `osfui.ready` simply never resolves and
`osfui.request()` rejects with code `"no-bridge"`.)

In-game, watch `Documents\My Games\Starfield\SFSE\Logs\OSF UI.log`:
- `MessageBridge: [web] ...` — your `log` commands.
- `MessageBridge: rejected unknown ui.command '...'` — you sent a non-whitelisted command.
- `UltralightWebRenderer: [console] ...` — your `console.log/warn/error` is forwarded here.
- Set `devMode: true` in `config.json` for verbose per-call logging and a
  first-frame PNG dump under `OSFUI/ultralight/`.

With `devMode: true` the in-game loop is alt-tab fast too:
- **Settings schemas hot-reload**: edits to `settings/*.json` are picked up
  within ~1 s — values are preserved (a renamed key carries over via its
  `aliases`), an open settings view repaints itself, and deleting the file
  drops the mod. A runtime-registered (DLL) schema is never touched by files.
- **View reload key** (`devReloadKey`, default `F11`): reloads the top open
  menu's URL in place, so HTML/JS/CSS edits show up without relaunching. The
  key is consumed by the framework while devMode is on.

---

## 6. Checklist for shipping a view

- [ ] `views/<modId>/<viewName>/manifest.json` — folder names pass the id grammar (§0), manifest `id` equals the view folder name, `permissions.nativeBridge` set as needed.
- [ ] Responsive CSS (no hardcoded 1280×720 assumptions; the view is resized to the screen).
- [ ] All assets local and relative (no `..`, no absolute paths, no network) — plus the sanctioned `../../shared/osfui.css` / `../../shared/osfui.js`.
- [ ] Load `shared/osfui.js` before your script; boot off `osfui.ready`, gate features with `osfui.has()`.
- [ ] Only whitelisted `ui.command`s; use `osfui.request()` (and its rejection `code`) where the outcome matters.
- [ ] (If configurable) a `settings/<modId>.json` schema with sane `default`/`min`/`max`.
- [ ] Verified standalone in a browser, then in-game via the log.

---

## 7. Schemas & type definitions

Tooling to author against the contract instead of from memory:

- **JSON Schemas** ([`docs/schema/`](schema/)) validate your files in any
  editor that understands JSON Schema (e.g. VS Code):
  - [`manifest.schema.json`](schema/manifest.schema.json) — `views/<modId>/<viewName>/manifest.json`
  - [`settings-schema.schema.json`](schema/settings-schema.schema.json) — `settings/<id>.json`

  Point your editor at them (VS Code `json.schemas`, or a top-level `"$schema"`
  key in your file) for autocomplete and validation.

- **TypeScript definitions** ([`sdk/osfui.d.ts`](../sdk/osfui.d.ts))
  type `window.osfui`, the `ui.command` whitelist, the native→web messages,
  and the settings-schema shapes. Reference it from your view's TS project and
  the bridge is typed globally — no package to install.

### Versioning & feature detection

**Gate on capabilities, not version arithmetic.** `runtime.ready` carries
`capabilities: string[]` — append-only named features (a capability, once
shipped, is never removed or renamed): surface names (`settings`,
`settings.captureKey`, `views`, `game.calendar`, `gamepad`), `request-id`
(the `ui.result` envelope), `schema:requires`, and `type:<t>` per setting
value type. It is the **same vocabulary** as a settings schema's `requires`
array, so one name answers both "can this host render my schema" and "can my
view use this feature":

```js
const info = await osfui.ready;
if (!osfui.has("settings")) {
  showError(`This OSF UI (${info.version}) has no settings surface.`);
} else if (osfui.has("type:flags")) {
  // safe to offer the multi-select UI
}
```

The protocol version is **1.0**, emitted as `bridgeVersion` — informational
(logs, bug reports), distinct from the plugin `version`. From 1.0 the
contract is stable: additive changes bump the minor and announce themselves
as new capabilities; anything that would break a shipped view bumps the
major. The constant lives in `src/core/Version.h`
(`kBridgeProtocolVersion`); the schemas, `.d.ts`, and the shared helper are
kept in lockstep with it (CI greps the docs against the constant).
