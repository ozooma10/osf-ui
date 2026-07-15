# Authoring Views & Settings

How to build a UI for OSF UI without touching the C++ runtime. This is
a reference for the two data-driven extension points that work today:

1. **Views** — an HTML/CSS/JS page rendered as an in-game overlay.
2. **Settings schemas** — typed knobs that appear in the built-in `settings`
   view (MCM-style), persisted and validated natively.

> **Status / scope.** Today the runtime renders **one view at a time**
> (`config.json` → `view`), and new *native* bridge commands or game-data
> bindings still require core changes. What you can ship as pure content with
> no recompile: a `views/<id>/` folder and a `settings/<id>.json` schema. The
> bridge protocol is at version **0.4 — unstable**; minor bumps may break
> views until it reaches 1.0. Detect it via the `bridgeVersion` field of the
> `runtime.ready` handshake (below) and degrade/refuse on a mismatch. See the
> roadmap in [renderer-plan.md](renderer-plan.md).

Everything here assumes you have read [security-model.md](security-model.md):
**your view is treated as untrusted code.** There is no network, no filesystem
beyond your own folder, and no way to call arbitrary native functions.

---

## 1. View package layout

A view is a folder under the plugin data dir:

```
SFSE/Plugins/OSFUI/views/<your-id>/
  manifest.json     required — declares the view
  index.html        your entry page (name configurable via manifest "entry")
  style.css         (optional) your styles
  main.js           (optional) your logic
  assets/...        (optional) images/fonts — local only
```

The folder is discovered automatically at load. To make it the active view,
set `view` in `config.json` to your manifest `id`.

All asset references must be **relative and stay inside your folder**. Absolute
paths, paths with a root, and any `..` component are rejected before disk I/O
(`SandboxFileSystem`, enforced in the renderer). The page is loaded as
`file:///<entry>`.

---

## 2. `manifest.json` reference

```jsonc
{
  "id": "myhud",            // REQUIRED, unique; matches the folder + config "view"
  "title": "My HUD",        // optional, defaults to id
  "description": "",        // optional; one-line blurb shown in catalogs (views.data / the hub view)
  "hub": true,              // optional, default true; false = hidden utility view — loads and works, but isn't advertised in catalogs
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
- **`width`/`height` are the page's LOGICAL size — author against it.** When the
  `d3d12` compositor is active, the runtime resizes the view to match the screen
  aspect (height-capped at 1440) **with a matching device scale**
  (`outputHeight / height`), so the page always lays out at its logical height
  and CSS px scale up to output pixels. In practice: at 1440p a 720-tall
  manifest gets a 2.0 device scale and a CSS viewport 720 tall — type sized for
  720p stays that size on screen instead of shrinking. Width still varies with
  the screen's aspect ratio (e.g. ~1720 CSS px wide on a 21:9 display), so
  author width-responsive CSS; the logical HEIGHT is the only fixed contract.
- **`permissions.nativeBridge` must be `true`** if your page talks to the
  runtime. With it `false`, `window.osfui` is never injected and your page
  runs purely client-side.
- A manifest that fails validation (missing `id`, escaping `entry`) is skipped
  with an error in `OSF UI.log`.

### Multiple views & layering

Several views can be hosted and composited at once. `config.json` lists them:

```jsonc
{
  "view": "settings",                 // the ACTIVE view: receives input + the bridge
  "views": ["settings", "hud"]        // the set of views to load (membership only)
}
```

- **Layering** is by each view's manifest `zorder` (not the array order): lower
  draws beneath, higher on top; ties keep load order. A HUD with `zorder: 100`
  always sits above a `zorder: 0` menu.
- **Focus / input** starts on `config.view`, which must be an `interactive`
  view. A view with `interactive: false` (a passive HUD) is never focused and
  never receives input, even when it is the top layer.
- **Switching focus:** when more than one `interactive` view is hosted, the
  `focusKey` (default `Tab`) cycles which one receives input. Only the focused
  view gets keyboard/mouse; the others keep rendering. So you can have several
  interactive panels and Tab between them.
- **Each bridge-enabled view (`nativeBridge: true`) has its own bridge.**
  Messages are attributed to their source view and replies route back to it, so
  several views can talk to native independently — even a passive HUD can
  receive native pushes and post messages. Input goes to one view at a time (the
  focused one) and the `focusKey` switches between interactive views, so a second
  *interactive* (clickable) view works too: focus it, then click.
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
scripts run:

```js
// web → native: send one JSON message
window.osfui.postMessage(jsonString);

// native → web: you assign this; the runtime calls it with a JSON string
window.osfui.onMessage = (jsonString) => { ... };
```

`postMessage` is read-only and cannot be reassigned. You provide `onMessage`.
Messages sent before your `onMessage` exists are queued (bounded, FIFO) and
flushed once the DOM is ready **and** `onMessage` is installed, so it is safe
to assign it at the top of your script. Do assign it during initial script
execution: a view that has queued messages it cannot yet receive is kept off
screen until they are delivered (the plugin-API "open a view in a specific
state" guarantee, ABI 1.3 — see `docs/native-plugin-api.md` §6a), and a page
that never installs `onMessage` while being sent messages stays hidden with a
warning in the SFSE log.

### Message envelope

Every message in both directions is JSON text with this shape:

```jsonc
{ "type": "<string>", "payload": { ... } }
```

Malformed messages are rejected and logged, never fatal. Both directions are
capped at 64 queued messages (excess is dropped with a warning) — **do not
flood the bridge**; it shares the game thread.

### Web → native

There is exactly **one** accepted inbound type: `ui.command`. The `payload`
carries a `command` field plus that command's arguments:

```js
function send(command, fields = {}) {
  window.osfui.postMessage(JSON.stringify({
    type: "ui.command",
    payload: { command, ...fields },
  }));
}
```

Whitelisted commands (anything else is rejected + logged):

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
| `settings.captureKey` | `mod, key` | arm native key-rebind capture for ANY `key`-typed setting of any mod; the next key press replies with `settings.captured`. Captured natively so pressing the current toggle key rebinds instead of closing the overlay |

> There is intentionally **no** "call any native function" escape hatch. New
> commands come from native code only: either a handler in the OSF UI runtime,
> or a **separate SFSE plugin** registering its own commands through the native
> bridge API ([native-plugin-api.md](native-plugin-api.md) — reserved prefixes
> `ui.`/`runtime.`/`game.`/`settings.`/`views.` are refused).

### Native → web

Assign `window.osfui.onMessage` and switch on `message.type`:

| type | payload | when |
|---|---|---|
| `runtime.ready` | `{ game, plugin, version, bridgeVersion }` | once, after your page loads — your cue to request data and to check `bridgeVersion` |
| `runtime.pong` | `{}` | reply to your `ping` |
| `game.data` | `{ available, day, month, year, hour, daysPassed }` | reply to `game.get`; `available:false` before a save is loaded |
| `views.data` | `{ views: [ { id, title, description, kind, interactive, hub, open, focused, loadState } ] }` | reply to `views.get`, and re-pushed to every subscribed view when any entry changes. `kind` = `"menu"`\|`"hud"`; `loadState` = `"loading"`\|`"loaded"`\|`"failed"`; a view torn down by crash-recovery drops out of the list. Respect `hub:false` (don't list those) |
| `settings.data` | `{ mods: [ { id, title, schema, values } ], vanillaKeys? }` | reply to `settings.get` / after a `settings.reset`. A `key`-typed setting whose binding collides with another mod's carries runtime-injected `conflicts: [{mod, key, title}]` in its schema object — informational; render a badge, never block. `mod` may be the reserved id `@game` (the game's own bindings; display `title`, e.g. "Starfield (Quicksave)"). Top-level `vanillaKeys: [{event, title, name}]` is the game's FULL binding table (read-only; the keybinds view renders it); absent when the runtime has none |
| `settings.ack` | `{ mod, key, ok }` | result of a `settings.set` (`ok:false` ⇒ rejected/clamped) |
| `settings.captured` | `{ mod, key, name, cancelled, conflicts? }` | reply to `settings.captureKey`: the captured key `name` (an OSF UI key name), or `cancelled:true` (Escape / unbindable — keep the old binding). When the captured key is already bound elsewhere, `conflicts: [{mod, key, title}]` lists the collisions this bind would create — warn live, never block. The view then sends a normal `settings.set` with `name` |
| `ui.hotkey` | `{ mod, key }` | the physical key currently bound to that `key`-typed setting was pressed in-game (protocol 0.4). Pushed to every `settings.get` subscriber — filter on `mod`. Suppressed while the overlay captures input or a rebind is armed; rebinds re-route automatically |
| `ui.error` | `{ reason, type?, command? }` | the runtime rejected something you sent — a malformed message, an unknown `type`, or an unknown `command`. Log it while developing; the same WARN is in `OSF UI.log` |

Unknown `type`s should be ignored (never `eval`'d) — including future `type`s
this runtime version doesn't know about.

### Minimal example

```js
"use strict";
const bridge = () =>
  typeof window.osfui === "object" &&
  typeof window.osfui.postMessage === "function";

function send(command, fields = {}) {
  if (bridge()) window.osfui.postMessage(
    JSON.stringify({ type: "ui.command", payload: { command, ...fields } }));
}

window.osfui = window.osfui || {};
window.osfui.onMessage = (json) => {
  const msg = JSON.parse(json);
  if (msg.type === "runtime.ready") {
    document.title = `Connected to ${msg.payload.plugin} v${msg.payload.version}`;
  }
};

document.getElementById("close").onclick = () => send("close");
```

See [`data/OSFUI/views/test/main.js`](../data/OSFUI/views/test/main.js)
for a complete, commented example.

---

## 4. Settings schemas (the MCM-style path)

Drop a JSON schema at:

```
SFSE/Plugins/OSFUI/settings/<your-mod-id>.json
```

Every schema in that folder is loaded as a separate "mod" and rendered as its
own card in the built-in `settings` view — **with zero per-mod native or web
code**. Values persist per-mod to a user-writable path
(`Documents\My Games\Starfield\OSFUI\settings\<id>.json`), survive
relaunch, and the runtime can react to changes natively.

### Schema format

```jsonc
{
  "id": "mymod",                 // optional, defaults to the filename
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

### Type rules (enforced natively in `SettingsStore`)

| type | control | validation on write |
|---|---|---|
| `bool` | checkbox | must be a boolean |
| `int` | slider | number, clamped to `[min,max]`, rounded |
| `float` | slider | number, clamped to `[min,max]` |
| `enum` | dropdown | must be one of `options` |
| `string` | text field | truncated to 256 chars |
| `key` | press-to-bind button | key-name string (≤16 chars), non-empty unless the setting sets `"allowUnbound": true` — then `""` is the deliberate unbound state (no hotkey dispatch, no conflict badges, and the UI adds an unbind ×). **Framework-managed:** capture is armed via `settings.captureKey` and grabbed in the native input layer (so pressing the current toggle key rebinds instead of closing the overlay). Every `key`-typed setting of every mod is rebindable and dispatches via the HotkeyService (`ui.hotkey` / `SubscribeHotkey`) |

The `bool` control renders as a toggle switch, `int`/`float` as sliders with a
value badge — see `../shared/osfui.css` for the shared control styles.

Unknown keys, wrong types, and out-of-range values are rejected/clamped
server-side and reported via `settings.ack {ok:false}`. **Untrusted JS cannot
write arbitrary keys, out-of-range values, or to any path but its own settings
file.**

### Reacting to changes from your view (zero native code)

`settings.get` doesn't just read — it **subscribes** the calling view (the
`views.get` pattern). After one `settings.get` at startup:

- every committed value — from the settings menu, a preset, a reset, or a
  native write — is pushed to you as
  `settings.changed { mod, key, value }` (the value is post-validation, i.e.
  authoritative), and
- a registry *shape* change (a mod registering/unregistering a schema at
  runtime) re-sends the full `settings.data`.

So a mod's HUD view reacts live to its own settings with zero polling:

```js
osfui.onMessage = (json) => {
  const msg = JSON.parse(json);
  if (msg.type === "settings.changed" && msg.payload.mod === "mymod") {
    applySetting(msg.payload.key, msg.payload.value);  // e.g. re-style, re-layout
  }
};
send("settings.get");  // initial read + subscription in one call
```

You receive changes for **all** mods — filter on `payload.mod`.

### Hotkeys with zero native code (protocol 0.4)

Every `type:"key"` setting is a **live hotkey** (mcm-design.md §9): when the
user presses the bound key in-game, the runtime pushes
`ui.hotkey { mod, key }` to every `settings.get` subscriber. So the same
single subscription above also makes your HUD toggleable:

```js
if (msg.type === "ui.hotkey" && msg.payload.mod === "mymod" && msg.payload.key === "toggleHud") {
  send("setViewHidden", { hidden: visible = !visible });  // or hud.show/hud.hide
}
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
if (!bridge()) {
  // running in a plain browser — stub or no-op the native calls
}
```

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

- [ ] `views/<id>/manifest.json` with a unique `id` and `permissions.nativeBridge` set as needed.
- [ ] Responsive CSS (no hardcoded 1280×720 assumptions; the view is resized to the screen).
- [ ] All assets local and relative (no `..`, no absolute paths, no network).
- [ ] `onMessage` assigned before you send anything; handle `runtime.ready`.
- [ ] Only whitelisted `ui.command`s; handle `*.ack`/error replies.
- [ ] (If configurable) a `settings/<id>.json` schema with sane `default`/`min`/`max`.
- [ ] Verified standalone in a browser, then in-game via the log.

---

## 7. Schemas & type definitions

Tooling to author against the contract instead of from memory:

- **JSON Schemas** ([`docs/schema/`](schema/)) validate your files in any
  editor that understands JSON Schema (e.g. VS Code):
  - [`manifest.schema.json`](schema/manifest.schema.json) — `views/<id>/manifest.json`
  - [`settings-schema.schema.json`](schema/settings-schema.schema.json) — `settings/<id>.json`

  Point your editor at them (VS Code `json.schemas`, or a top-level `"$schema"`
  key in your file) for autocomplete and validation.

- **TypeScript definitions** ([`sdk/osfui.d.ts`](../sdk/osfui.d.ts))
  type `window.osfui`, the `ui.command` whitelist, the native→web messages,
  and the settings-schema shapes. Reference it from your view's TS project and
  the bridge is typed globally — no package to install.

### Versioning

The protocol version is **0.4** and is emitted in `runtime.ready` as
`bridgeVersion`. It is distinct from the plugin `version`. Until it reaches
`1.0`, treat minor bumps as potentially breaking and gate your view on it:

```js
window.osfui.onMessage = (json) => {
  const msg = JSON.parse(json);
  if (msg.type === "runtime.ready" && !msg.payload.bridgeVersion?.startsWith("0.")) {
    // a newer, possibly incompatible runtime — warn or refuse rather than guess
  }
};
```

The constant lives in `src/core/Version.h` (`kBridgeProtocolVersion`); the
schemas and `.d.ts` are kept in lockstep with it.
