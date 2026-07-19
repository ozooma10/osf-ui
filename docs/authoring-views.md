# Authoring Views & Settings

> **Disclaimer:** This document is AI-generated (written with Claude and
> reviewed against the source code). If it ever disagrees with the code, the
> JSON Schemas (§7), or `sdk/osfui.d.ts`, those are authoritative — and a bug
> report about the mismatch is welcome.

How to build a UI for OSF UI without touching the C++ runtime. This is
a reference for the two data-driven extension points that work today:

1. **Views** — an HTML/CSS/JS page rendered as an in-game overlay.
2. **Settings schemas** — typed settings that appear in the built-in `settings`
   view (MCM-style), persisted and validated natively.

> **Status / scope.** Pure content, no recompile: a
> `views/<modId>/<viewName>/` folder and a `settings/<modId>.json` schema. The
> bridge protocol is at version **1.0 — stable**; additive changes bump the
> minor version, breaking changes bump the major. Compatibility is advisory:
> declare the OSF UI version you authored against as `targetVersion` (manifest
> and/or settings schema, §7), and the Mods surface shows a "needs update"
> badge when the running host is older. `bridgeVersion` is informational. See
> [ROADMAP.md](ROADMAP.md) for what is still planned.

## 0. Identifiers

Every public identifier derives from your mod id:

- **Mod id** — `<author>.<modname>`, e.g. `ozooma10.almanac`. Lowercase
  `[a-z0-9-]` segments, exactly one dot, max 64 chars. The author segment is a
  handle you already own (your Nexus or GitHub username); it is
  self-allocated, with no registry. Dotless ids are reserved for the platform
  (`osfui` is the only dotless built-in), so there is no reserved-word list to
  collide with.
- **View name** — `[a-z0-9-]+`, local to your mod (`planets`).
- **Qualified view id** — `<modId>/<viewName>` (`ozooma10.almanac/planets`).
  This is the id used everywhere views are referenced: `config.json`
  `view`/`views`, `menu.open`, `views.data`, `RegisterView`. The slash mirrors
  the folder path; a dotted join would be ambiguous because mod ids already
  contain a dot.

Ids that fail the grammar are rejected at load, with an ERROR in `OSF UI.log`
naming the file and the rule. The same mod id names your settings schema
(`settings/<modId>.json`), your values file, your view namespace folder, and
the prefix of your native bridge commands (`<modId>.<command>`) — one identity
across every surface.

Two author prefixes are already claimed: `osfui` belongs to the platform
(reserved, dotless) and `osf` to the OSF family of mods (`osf.animation`, plus
future `osf.*` siblings). Don't publish under an author segment someone else
already ships under; when two mods collide, whichever loads first wins.

Before authoring, read [security-model.md](security-model.md): your view is
treated as untrusted code. There is no network access, no filesystem access
beyond your own folder, and no way to call arbitrary native functions.

---

## 1. View package layout

A view is a folder inside your mod's namespace folder under the plugin
data dir:

```
SFSE/Plugins/OSFUI/views/<author>.<modname>/<viewname>/
  manifest.json     required — declares the view
  index.html        your entry page (name configurable via manifest "entry")
  style.css         (optional) your styles
  main.js           (optional) your logic
  assets/...        (optional) images/fonts — local only
```

The two-level layout is discovered automatically at load. A mod folder may
hold several views, and subfolders without a `manifest.json` are ignored, so
you can keep shared assets next to your views. The built-in views use the
same layout: `views/osfui/settings/`, `views/osfui/keybinds/`. To open your
view, use its qualified id `<modId>/<viewName>` — as a `config.json` `views`
entry, a `menu.open` target, or a `RegisterView` argument.

All asset references must be relative: absolute paths, paths with a root,
and any `..` component are rejected before disk I/O (`SandboxFileSystem`,
enforced in the Ultralight renderer), which confines every request to the
views root. Keep your references inside your own folder — the only
cross-folder path that is part of the contract is the shared UI kit at
`views/shared/osfui.css` / `osfui.js`; link those as
`../../shared/osfui.css`.

### The page URL depends on the renderer backend

The views root is the URL root on both backends, and your view is always at
`<modId>/<viewName>/<entry>` under it — but the scheme is not the same:

| `renderer` | Your page's URL | Origin |
|---|---|---|
| `webview2` (the shipped default) | `https://osfui.local/<modId>/<viewName>/<entry>` | a normal https origin, mapped to the views folder by `SetVirtualHostNameToFolderMapping` |
| `ultralight` | `file:///<modId>/<viewName>/<entry>` | opaque |

Either way `../../shared/osfui.css` collapses to the shared kit at the views
root (`https://osfui.local/shared/…` or `file:///shared/…`).

> **Portability rule: classic `<script src>` only.**
> `type="module"`, dynamic `import()` and `fetch()` all work on WebView2 and
> all fail under Ultralight's opaque `file://` origin, where module and fetch
> requests are CORS-checked against an origin that can never match. Ultralight
> is the compatibility floor — it is what 1.0.0-alpha shipped and it remains
> selectable — so a view that needs both backends must use plain classic
> scripts, no module type, no code splitting, and must not `fetch()` its own
> assets (inline the data or emit it into the bundle instead). The failure
> mode is the expensive kind: a blank view and a console error nobody reads.
> The built-in views are held to this by build gates
> (`frontend/scripts/verify-output.mjs`); yours are on the honour system.

### The shared UI kit

`shared/osfui.css` is the design system every shipped view uses. Its
contract:

- Everything the kit exports is prefixed: classes `osf-*`, custom properties
  `--osf-*`. Nothing un-prefixed is part of the contract, so your own class
  and token names cannot collide with a kit update.
- Linking the sheet is opt-in, and it is all-or-nothing. It styles
  element-level bases (body, headings, `a`, `kbd`, `::selection`, scrollbars,
  form elements) globally, as any design-system base sheet does. Link it for
  the native look, or don't link it and own all your styling; there is no
  partial mode.
- Theming is a single accent color. There are no theme classes; a mod's
  accent is the `accent` value in its schema or manifest. Apply it to any
  subtree with `osfui.applyAccent(el, "#e6904a")` (from `shared/osfui.js`),
  which derives the kit's linked accent set (`--osf-accent`, `-hover`,
  `-strong`, `-quiet`). A missing or invalid value clears it.

### `padnav.js` is not part of the shared kit

The built-in views ship a third script, `views/osfui/padnav.js`, which does
spatial gamepad/focus navigation. It is **private to the `osfui` views**, by
its own declaration — it is not published, not versioned, not frozen, and
explicitly reserves the right to change shape freely. Two consequences:

- **Don't link it.** It lives under the `osfui/` mod namespace, not under
  `shared/`, so reaching it would need the `..` that the sandbox rejects. Only
  `shared/osfui.css` and `shared/osfui.js` are contract paths.
- **Don't depend on its DOM conventions.** It navigates by reading concrete
  geometry and class names (`.row` bands, `.listening`, `[data-nav-modal]`).
  Those are an internal arrangement between padnav and the built-in views, not
  an API.

Native gamepad→UI mapping (D-pad and left stick→arrows, A→Enter, B→back, right
stick→scroll) is delivered by the runtime to *every* view regardless, so basic
controller use works without padnav. If you want richer focus handling, own it
in your own script — or take raw events with `osfui.gamepadRaw` (§3).
See [`frontend/COMPATIBILITY.md`](../frontend/COMPATIBILITY.md) §3.

### Localization

Write normal English in your manifest and page. Community translation mods
ship `SFSE/Plugins/OSFUI/l10n/<modId>_<locale>.json`; you don't need an
English catalog or manual string keys. Manifest metadata is addressed as
`views.<viewName>.title` and `views.<viewName>.description` automatically.

For custom page chrome, use a stable address plus inline English:

```js
heading.textContent = osfui.t("views.myhud.heading", "Ship status");
count.textContent = osfui.t("views.myhud.itemCount", "{count} items", { count: items.length });
```

Static markup can be translated without page code:

```html
<h1 data-i18n="views.myhud.heading">Ship status</h1>
<input placeholder="Search" data-i18n-placeholder="views.myhud.search">
```

The shared helper requests the calling mod's catalog after `runtime.ready`,
sets `document.documentElement.lang`, reapplies `data-i18n*` on every live
language change, and exposes `osfui.i18nReady` for code that must wait for
the first catalog. Lookup falls back per string: exact locale, base locale,
an optional English override, then the inline English.

---

## 2. `manifest.json` reference

```jsonc
{
  "id": "myhud",            // required; must equal the view folder name. The runtime id is the qualified "<modId>/myhud", derived from the path
  "title": "My HUD",        // optional, defaults to the qualified id
  "description": "",        // optional; one-line blurb shown in catalogs (views.data / the Mods surface)
  "hub": true,              // optional, default true; false = hidden utility view — loads and works, but isn't advertised in catalogs (name predates the Mods surface)
  "targetVersion": "1.0.0", // optional; the OSF UI version this view is authored against — advisory, never gates loading (see note below)
  "entry": "index.html",    // optional, default "index.html"; must stay inside the folder
  "width": 1600,            // optional, default 1600; clamped to 1..16384 — logical (authoring) size
  "height": 900,            // optional, default 900;  clamped to 1..16384 — logical (authoring) size
  "transparent": true,      // optional, default true; lets the game show through
  "permissions": {          // optional; everything defaults to DENY
    "nativeBridge": true,   // false ⇒ no window.osfui bridge is created at all
    "filesystem": false,    // reserved; no effect yet
    "network": false         // reserved; forced off with a warning if set true
  }
}
```

Notes:
- Unknown manifest keys are ignored, so a manifest written for a newer OSF UI
  parses leniently (devMode logs them at INFO). An optional
  `"manifestVersion"` integer is accepted but not required; the nested folder
  layout itself identifies the format.
- `targetVersion` declares the OSF UI version your view was authored against
  (`"<major>[.<minor>[.<patch>]]"`, e.g. `"1.1.0"`). It is advisory: the view
  always loads and does what it can. When the running OSF UI is older than
  the target, a warning is written to `OSF UI.log` and the Mods surface shows
  a "needs update" badge next to the OSF UI version number, with your mod
  named in the tooltip — so the user learns OSF UI is what needs updating,
  instead of blaming your mod for missing features. A malformed value is
  ignored with a warning. Degrade gracefully at runtime regardless (compare
  `runtime.ready`'s `version` if you must branch); `targetVersion` informs
  the user, not your code.
- `width`/`height` set the page's logical size; author against it. When the
  `d3d12` compositor is active, the runtime resizes the view to match the
  screen aspect (height capped at 1440) with a matching device scale
  (`outputHeight / height`), so the page always lays out at its logical
  height and CSS pixels scale up to output pixels. At 1440p, a 720-tall
  manifest gets a 2.0 device scale and a CSS viewport 720 px tall — type
  sized for 720p stays that size on screen instead of shrinking. Width still
  varies with the screen's aspect ratio (about 1720 CSS px wide on a 21:9
  display), so write width-responsive CSS. The versioned layout guarantee
  (protocol 1.0) is that your manifest's logical height is fixed; width is
  not.
- `permissions.nativeBridge` must be `true` if your page talks to the
  runtime. When it is `false`, `window.osfui` is never injected and your page
  runs purely client-side.
- A manifest that fails validation (`id` not matching the folder name, an
  `entry` escaping the folder, a folder name violating the id grammar) is
  skipped with an error in `OSF UI.log`. The owning mod id is taken from the
  `views/<modId>/` folder, so a view always groups onto its own mod's page
  automatically.

### Multiple views & layering

Several views can be hosted and composited at once. `config.json` lists them
by qualified id:

```jsonc
{
  "view": "osfui/settings",                          // the ACTIVE view: receives input + the bridge
  "views": ["osfui/settings", "yourname.mymod/hud"]  // the set of views to load (membership only)
}
```

> Shipping a view with a native mod? Don't edit the user's `config.json`.
> Your SFSE plugin can register its shipped `views/<modId>/<viewName>/`
> folder at runtime with one bridge call
> (`RegisterView("<modId>/<viewName>")`, C ABI 1.5). The view then joins the
> views catalog: it appears on the Mods surface as a launch card on the Home
> page and on its mod's page (grouped by the `views/<modId>/` folder it lives
> under), and opens via `RequestMenu` / `menu.open`. See
> [native-plugin-api.md](native-plugin-api.md) §5c. The `views` array is for
> the user's own composition (and OSF UI's built-ins).

- Layering is set by the menu/HUD framework, not the array order: every HUD
  composites beneath every open menu. HUDs order among themselves by their
  manifest `order` (higher on top, clamped 0..999); open menus stack in the
  order they were opened, top menu on top. An open menu therefore always sits
  above any HUD, whatever the HUD's `order`.
- The focus model is a versioned guarantee (protocol 1.0). Input goes to
  exactly one view: the top open menu (the stack holds a single menu, so
  opening a menu replaces and focuses it). A `"kind": "hud"` view is passive:
  it is never focused and never receives input, even when it is the top
  layer. There is no user-facing focus-cycle key.
- Each bridge-enabled view (`nativeBridge: true`) has its own bridge.
  Messages are attributed to their source view and replies route back to it,
  so several views can talk to native independently; even a passive HUD can
  receive native pushes and post messages.
- Each view is sized to the whole screen, so position your content with CSS
  and keep the rest transparent. The layers blend by alpha.

### Mouse & cursor

Don't draw your own pointer. While the overlay captures input, the runtime
shows the real Windows hardware cursor: zero lag, composited by the display
hardware, independent of game framerate. Your page's CSS `cursor` property
maps to the matching system cursor (`pointer` → hand, `text` → I-beam, the
resize variants; `none` hides it; anything exotic falls back to the arrow),
so hover feedback works exactly like a browser. A page-drawn `<div>` pointer
would trail the real one by the full render pipeline — the shipped views used
to do this, and it was removed for feeling laggy — and `cursor: none` on
`body` would hide the pointer for the whole view.

---

## 3. The bridge — `window.osfui`

When `nativeBridge` is granted, the runtime injects one object before your
page scripts run. Use it through the shipped helper, loaded like the shared
stylesheet, before your own script:

```html
<script src="../../shared/osfui.js"></script>
<script src="main.js"></script>
```

```js
// The helper's full surface (deliberately thin — it is part of the contract):
osfui.available()                 // bridge present? false = plain browser
const info = await osfui.ready;   // the runtime.ready payload (info.version = the running OSF UI)
osfui.send("close");              // fire-and-forget ui.command
const reply = await osfui.request("settings.get");   // correlated request
const off = osfui.on("settings.changed", (payload) => { ... });  // subscribe
```

`osfui.request()` generates a `requestId` and resolves with the reply message
(`{ type, requestId, payload }`), or rejects with an `Error` carrying a
stable `.code`: on `ui.error`, on `ui.result { ok:false }`, on timeout
(default 10 s; pass `{ timeoutMs: 0 }` to disable, e.g. for a key capture
that waits on the user), and immediately when no bridge is present. With the
helper loaded it owns `osfui.onMessage`; subscribe with `osfui.on()` and
never assign `onMessage` yourself.

Under the helper sit two primitives (all the helper itself uses):

```js
// web → native: send one JSON message
window.osfui.postMessage(jsonString);

// native → web: the runtime calls it with a JSON string (owned by the helper)
window.osfui.onMessage = (jsonString) => { ... };
```

`postMessage` is read-only and cannot be reassigned. Messages sent before
`onMessage` exists are queued (bounded, FIFO) and flushed once the DOM is
ready and `onMessage` is installed; loading `osfui.js` in `<head>` or before
your script satisfies this. A view with queued messages it cannot yet receive
is kept off screen until they are delivered (the plugin-API "open a view in a
specific state" guarantee, ABI 1.3 — see
[native-plugin-api.md](native-plugin-api.md) §6a), and a page that never
installs `onMessage` while being sent messages stays hidden, with a warning
in the SFSE log.

### Message envelope

Every message in both directions is JSON text with this shape:

```jsonc
{ "type": "<string>", "requestId": "<optional string>", "payload": { ... } }
```

`requestId` (protocol 1.0) is the correlation contract, and `osfui.request()`
handles it for you: any `ui.command` may carry a caller-chosen id (string,
1–64 chars). Every reply echoes the id top-level, and a command with no reply
type of its own (`close`, `menu.open`, …) answers
`ui.result { ok, command, code?, message? }` — but only when an id was
supplied. Omitting the id makes the command fire-and-forget.

Malformed messages are rejected and logged, never fatal. Both directions are
capped at 64 queued messages; excess is dropped with a warning. Don't flood
the bridge — it shares the game thread.

### Web → native

There is exactly one accepted inbound type: `ui.command`. The `payload`
carries a `command` field plus that command's arguments; `osfui.send(command,
fields)` / `osfui.request(command, fields)` build it for you.

Whitelisted commands (anything else is rejected, logged, and answered with
`ui.error { code: "unknown-command" }`):

| command | payload fields | effect |
|---|---|---|
| `close` | — | close the calling surface (closing the last open menu hides the overlay; a coexisting live HUD stays up) |
| `setVisible` | `visible: bool` | open/close the calling surface |
| `menu.open` | `view?: string` | open a registered surface by id (omitted ⇒ the calling view). Opening a menu also focuses it: under the single-menu policy it replaces the current menu and becomes the input target |
| `menu.close` | `view?: string` | close a registered surface by id (omitted ⇒ the calling view) |
| `hud.show` / `hud.hide` | `view?: string` | aliases of `menu.open` / `menu.close`; a surface's kind (menu vs. HUD) is fixed by its manifest, not by which command you use |
| `setViewHidden` | `view?: string`, `hidden: bool` | show/hide one *loaded* view, independent of the global overlay toggle (omitted `view` ⇒ self) |
| `log` | `text: string` | write to `OSF UI.log` (truncated to 512 chars) |
| `ping` | — | runtime replies with `runtime.pong` |
| `game.get` | — | runtime replies with `game.data` (in-game date/time from the calendar) |
| `views.get` | — | runtime replies with `views.data` (catalog of loaded surfaces) and subscribes the caller: any later open/close/focus/load-state change pushes a fresh `views.data` unsolicited, so no polling is needed |
| `i18n.get` | `mod?` | runtime replies with `i18n.data` for the requested mod (omitted `mod` = the calling view's owner) and subscribes the caller to live language/catalog changes |
| `settings.get` | — | runtime replies with `settings.data` |
| `settings.set` | `mod, key, value` | set one schema-declared setting (validated) |
| `settings.reset` | `mod`, `key?` | reset one key, or the whole mod if `key` omitted |
| `settings.captureKey` | `mod, key` | arm native key-rebind capture for any `key`-typed setting of any mod; the next key press replies with `settings.captured`, echoing the arming `requestId` however many ticks later, so `osfui.request("settings.captureKey", …, {timeoutMs: 0})` awaits the whole rebind. One capture at a time; a second arm answers `ui.result { ok:false, code:"capture-busy" }`. Capture happens in the native input layer, so pressing the current toggle key rebinds instead of closing the overlay |
| `osfui.gamepadRaw` | `raw: bool` | *(experimental — exempt from the 1.0 stability guarantee until stabilized)* take over gamepad handling: suppress the default nav mapping and consume raw `ui.gamepad` events yourself. The grant is sticky per view — it survives overlay hide/show and clears only when your page (re)loads or the view is destroyed; other views never inherit it |
| `osfui.handleBack` | `handle: bool` | own the back action. While your menu is ACTIVE, Esc / gamepad B are delivered to your page as a synthetic Escape keydown/keyup instead of closing the top menu — handle it and decide: navigate (`menu.open`), dismiss an inner panel, or send `close`. Same sticky-per-view lifecycle as `osfui.gamepadRaw` (clears on page (re)load / view destroy — re-assert in your boot code). The overlay toggle key always closes natively, so a page that stops responding cannot strand the player |

> There is intentionally no "call any native function" escape hatch. New
> commands come from native code only: either a handler in the OSF UI
> runtime, or a separate SFSE plugin registering its own commands through the
> native bridge API ([native-plugin-api.md](native-plugin-api.md)). Plugin
> commands must be shaped `<author>.<modname>.<name>` (two dots minimum; the
> leading mod id follows the §0 grammar), so no mod can register any of the
> platform commands above.

### Native → web

Subscribe with `osfui.on(type, fn)`. Replies that resolve an
`osfui.request()` are also dispatched to subscribers, so one render path
serves both:

| type | payload | when |
|---|---|---|
| `runtime.ready` | `{ game, plugin, version, bridgeVersion }` | once, after your page loads (`osfui.ready` resolves with it) — your cue to request data. `version` is the running OSF UI (the reference point for `targetVersion`, §7); `bridgeVersion` is informational |
| `runtime.pong` | `{}` | reply to your `ping` |
| `game.data` | `{ calendar: { available, day, month, year, hour, daysPassed } }` | reply to `game.get`; each provider nests under its own object (future providers are siblings of `calendar`); `available:false` before a save is loaded |
| `views.data` | `{ views: [ { id, title, description, mod, kind, interactive, hub, targetVersion, open, focused, loadState } ] }` | reply to `views.get`, and re-pushed to every subscribed view when any entry changes. `id` is the qualified `<modId>/<viewName>`; `mod` = the owning mod id derived from the folder (a mod with no settings schema of that id just lists under its own title); `kind` = `"menu"`\|`"hud"`; `targetVersion` = the manifest's declared target (empty string when undeclared); `loadState` = `"loading"`\|`"loaded"`\|`"failed"`; a view torn down by crash-recovery drops out of the list. Respect `hub:false` (don't list those) |
| `i18n.data` | `{ mod, locale, strings }` | reply to `i18n.get`, then re-pushed to subscribed views after a language change or a dev-mode catalog reload; `strings` contains active-locale overrides keyed by stable structural address |
| `settings.data` | `{ mods: [ { id, title, schema, values, shadowed?, targetVersion? } ], vanillaKeys?, loadErrors? }` | reply to `settings.get` / after a `settings.reset`. Per-mod `shadowed?` lists drop-in schema files that also claimed this id and lost first-wins (render a conflict badge); `targetVersion?` is the schema's advisory authored-against version (§7), omitted when undeclared. A `key`-typed setting whose binding collides with another mod's carries runtime-injected `conflicts: [{mod, key, title}]` in its schema object — informational; render a badge, never block. `mod` may be the reserved id `@game` (the game's own bindings; display `title`, e.g. "Starfield (Quicksave)"). Top-level `vanillaKeys: [{event, title, name}]` is the game's full binding table (read-only; the keybinds view renders it); absent when the runtime has none. Top-level `loadErrors: [{kind, file, mod?, message}]` names settings files that failed to load — `schema-name` / `schema-parse` (file skipped) or `values-parse` (values quarantined as `<file>.bad`, defaults served); absent when everything loaded clean. The Mods surface renders these as a rail alert |
| `settings.ack` | `{ mod, key, ok, value?, code?, message? }` | result of a `settings.set`. `ok:true` carries `value`, the authoritative post-clamp committed value (compare with what you sent to detect clamping — no re-fetch needed); `ok:false` carries a stable `code`: `unknown-setting`, `read-only` (a setting type this host doesn't know), or `invalid-value` |
| `settings.captured` | `{ mod, key, name, cancelled, conflicts? }` | reply to `settings.captureKey`: the captured key `name` (an OSF UI key name), or `cancelled:true` (Escape / unbindable — keep the old binding). When the captured key is already bound elsewhere, `conflicts: [{mod, key, title}]` lists actionable collisions this bind would create; expected `@game` reuse from a `blocksGameplay` context is omitted. Warn live, never block. The view then sends a normal `settings.set` with `name` |
| `ui.hotkey` | `{ mod, key }` | the physical key currently bound to that `key`-typed setting was pressed in-game (protocol 1.0). Pushed to every `settings.get` subscriber — filter on `mod`. Suppressed while the overlay captures input or a rebind is armed; rebinds re-route automatically |
| `ui.result` | `{ ok, command?, code?, message? }` | the uniform outcome (protocol 1.0), sent only when your `ui.command` carried a `requestId`: verb commands (`close`, `menu.open`, …) answer `ok:true` on success; failures carry a stable `code` (`unknown-view`, `capture-busy`, `unknown-setting`, …). For a plugin-registered command, `ok:true` means delivered to the plugin's handler. `osfui.request()` consumes this for you |
| `ui.gamepad` | `{ kind:"button", button:{id, down} }` \| `{ kind:"stick", axes:{lx, ly, rx, ry} }` | *(experimental — gamepad navigation is being refined; exempt from the 1.0 stability guarantee until stabilized)* raw gamepad events to the ACTIVE view while the overlay captures input (per-kind nesting, protocol 1.0). The default nav mapping (D-pad and left stick→arrows, A→Enter, B→back — a synthetic Escape to the page under `osfui.handleBack`, otherwise close, right stick→scroll) also applies unless you asserted `osfui.gamepadRaw` |
| `ui.visibility` | `{ visible }` | the receiving view was shown/hidden with the overlay (edge-triggered). The reference views scope per-visit state off this |
| `ui.error` | `{ code, message, type?, command? }` | the runtime rejected something you sent. `code` is a stable machine string — `malformed-message`, `unknown-message-type`, `unknown-command`; `message` is the human sentence. Echoes your `requestId` when it could be read, so `osfui.request()` rejects with it. The same WARN is in `OSF UI.log` |

Ignore unknown `type`s (never `eval` them), including future `type`s this
runtime version doesn't know about.

### Minimal example

```html
<script src="../../shared/osfui.js"></script>
<script>
"use strict";
osfui.ready.then((info) => {
  document.title = `Connected to ${info.plugin} v${info.version}`;
  osfui.send("settings.get");  // read + subscribe
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

See [`frontend/src/views/osfui/settings/`](../frontend/src/views/osfui/settings/)
for a complete, commented example — that is the *source* of the built-in Mods
surface. The copy under `data/OSFUI/views/osfui/settings/` is generated build
output; read it if you want the exact shipped bytes, but never edit it.

---

## 4. Settings schemas (the MCM-style path)

> The full author guide for this — quickstart, every widget, presets,
> hotkeys, localization, update strategy, testing — is
> [authoring-settings.md](authoring-settings.md). This section is the
> protocol-level summary.

Drop a JSON schema at:

```
SFSE/Plugins/OSFUI/settings/<author>.<modname>.json
```

Every schema in that folder is loaded as a separate "mod" and rendered as its
own card in the built-in `settings` view, with no per-mod native or web code.
Values persist per mod to
`SFSE/Plugins/OSFUI/settings/values/<id>.json` (VFS-captured, so per-profile
under MO2), survive relaunch, and the runtime can react to changes natively.

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
| `flags` | checkbox group | array of `options` strings (multi-select). Unknown options and duplicates are filtered out, and the stored array is canonicalized to declared-option order |
| `string` | text field | truncated to 256 chars |
| `key` | press-to-bind button | key-name string (≤16 chars), non-empty unless the setting sets `"allowUnbound": true` — then `""` is the deliberate unbound state (no hotkey dispatch, no conflict badges, and the UI adds an unbind ×). Framework-managed: capture is armed via `settings.captureKey` and grabbed in the native input layer, so pressing the current toggle key rebinds instead of closing the overlay. Every `key`-typed setting of every mod is rebindable and dispatches via the HotkeyService (`ui.hotkey` / `SubscribeHotkey`) |

This is the frozen base type set. There is no `color` type; use
`type:"string"` + `widget:"color"`. Post-1.0 evolution is a base type plus a
`widget` hint and attributes; a genuinely new base type ships in a new OSF UI
version that your schema names via `targetVersion`.

**Forward compatibility.** A host that predates one of your setting types
renders that row read-only ("needs a newer OSF UI"), serves the schema
`default` to consumers, and preserves the user's saved value untouched — it
round-trips through every rewrite, and a newer host picks it back up. If
your schema uses anything newer than the OSF UI you tested against, declare
that version so older hosts tell the user to update:

```jsonc
{ "id": "yourname.mymod", "targetVersion": "1.1.0" }
```

It is the same advisory field as the view manifest's (§2): the schema still
loads best-effort, but the Mods surface shows the "needs update" badge with
your mod named in the tooltip, and the detail pane notes that some settings
may be unavailable until OSF UI is updated.

Values files carry two reserved `$`-prefixed meta keys owned by the host:
`$schemaVersion` (your schema's `version`, stamped on write) and
`$formatVersion` (the sparse encoding's own version). Never name a setting
with a leading `$`; unknown `$`-keys from newer hosts round-trip like any
preserved value.

The `bool` control renders as a toggle switch, `int`/`float` as sliders with
a value badge; see `views/shared/osfui.css` for the shared control styles.

Unknown keys, wrong types, and out-of-range values are rejected or clamped
server-side and reported via `settings.ack {ok:false}`. Untrusted JS cannot
write arbitrary keys, out-of-range values, or to any path but its own
settings file.

### Reacting to changes from your view

`settings.get` doesn't just read — it subscribes the calling view, the same
pattern as `views.get`. After one `settings.get` at startup:

- every committed value (from the settings menu, a preset, a reset, or a
  native write) is pushed to you as
  `settings.changed { mod, key, value, conflicts? }`. The value is
  post-validation, i.e. authoritative; on `key`-typed settings, `conflicts`
  is the recomputed collision list (protocol 1.0), so badge updates need no
  registry re-fetch.
- a registry *shape* change (a mod registering or unregistering a schema at
  runtime) re-sends the full `settings.data`.

So a mod's HUD view reacts live to its own settings with zero polling and
zero native code:

```js
osfui.on("settings.changed", (p) => {
  if (p.mod === "yourname.mymod") {
    applySetting(p.key, p.value);  // e.g. re-style, re-layout
  }
});
osfui.send("settings.get");  // initial read + subscription in one call
```

You receive changes for all mods; filter on `p.mod`.

### Hotkeys (protocol 1.0)

Every `type:"key"` setting is a live hotkey (mcm-design.md §9): when the user
presses the bound key in-game, the runtime pushes `ui.hotkey { mod, key }` to
every `settings.get` subscriber. The same single subscription above therefore
also makes your HUD toggleable:

```js
osfui.on("ui.hotkey", (p) => {
  if (p.mod === "yourname.mymod" && p.key === "toggleHud") {
    osfui.send("setViewHidden", { hidden: visible = !visible });  // or hud.show/hud.hide
  }
});
```

Presses never fire while the overlay is capturing input (typing in an
overlay text field, for example) or while a rebind capture is armed, key
repeats never fire, and the user can rebind freely; the runtime re-resolves
the binding on every change. Duplicate bindings across mods all
fire — the settings view badges them (the `conflicts` data above) but never
blocks them.

### Reacting natively (C ABI)

Separate SFSE plugins subscribe over the native bridge, with no core edit
needed: `SubscribeSettings` plus typed getters for values (C ABI 1.2), and
`SubscribeHotkey` for key presses (C ABI 1.4). See
[native-plugin-api.md](native-plugin-api.md) §5a/§5b and `docs/mcm-design.md`
§8.2/§9. In-tree framework knobs still react through
`Runtime::OnSettingChanged` (e.g. `osfui.toggleKey` live-rebinds the
overlay's open/close key).

---

## 5. Testing locally

**Your own view:** open its `index.html` directly in a normal browser and
iterate on layout and logic without launching the game. Detect the missing
bridge and fall back to a standalone mode:

```js
if (!osfui.available()) {
  // running in a plain browser — stub or no-op the native calls
}
```

(`shared/osfui.js` installs itself even without a bridge, so `osfui.available()`
/ `osfui.on()` are always safe to call; `osfui.ready` simply never resolves and
`osfui.request()` rejects with code `"no-bridge"`.)

Serve it over `http://` rather than opening it from `file://` if you can — a
browser's `file://` rules are stricter than Ultralight's and will reject some
things that work in game.

**The built-in views** (`osfui/settings`, `osfui/keybinds`) are no longer
openable this way: their shipped `index.html` is generated output, and the
dev-time mock bridge lives in the frontend project. Use the Vite harness
instead, which mounts them against a mock bridge that speaks the real
protocol:

```bat
npm --prefix frontend run dev
```

See [`frontend/README.md`](../frontend/README.md) for the deep-link URLs and
the locale/fixture/stage switches.

In-game, watch `Documents\My Games\Starfield\SFSE\Logs\OSF UI.log`:
- `MessageBridge: [web] ...` — your `log` commands.
- `MessageBridge: rejected unknown ui.command '...'` — you sent a non-whitelisted command.
- `UltralightWebRenderer: [console] ...` — your `console.log/warn/error` is forwarded here.
- Set `devMode: true` in `config.json` for verbose per-call logging and a
  first-frame PNG dump under `OSFUI/ultralight/`.

With `devMode: true` the in-game loop is fast too:
- **Settings schemas hot-reload**: edits to `settings/*.json` are picked up
  within about a second. Values are preserved (a renamed key carries over via
  its `aliases`), an open settings view repaints itself, and deleting the
  file drops the mod. A runtime-registered (DLL) schema is never touched by
  files.
- **View reload key** (`devReloadKey`, default `F11`): reloads the top open
  menu's URL in place, so HTML/JS/CSS edits show up without relaunching. The
  key is consumed by the framework while devMode is on. It reloads whatever is
  **on disk** — for your own hand-authored view that is the file you just
  edited, but for a built-in view that is generated output, so run
  `npm --prefix frontend run build` first or F11 will faithfully reload the
  previous bundle.

---

## 6. Checklist for shipping a view

- [ ] `views/<modId>/<viewName>/manifest.json` — folder names pass the id grammar (§0), manifest `id` equals the view folder name, `permissions.nativeBridge` set as needed.
- [ ] Responsive CSS (no hardcoded 1280×720 assumptions; the view is resized to the screen).
- [ ] All assets local and relative (no `..`, no absolute paths, no network) — plus the sanctioned `../../shared/osfui.css` / `../../shared/osfui.js`.
- [ ] Classic `<script src>` only — no `type="module"`, no dynamic `import()`, no `fetch()` (§1). Works on WebView2, silently blank under Ultralight.
- [ ] Load `shared/osfui.js` before your script; boot off `osfui.ready`. Declare `targetVersion` if you use anything newer than the OSF UI you tested against.
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
  and the settings-schema shapes. Reference it from your view's TS project
  and the bridge is typed globally — no package to install.

### Versioning

Declare what you authored against; degrade gracefully at runtime. One
advisory field, `targetVersion`, appears in both author-facing files (view
manifest §2, settings schema §4): the OSF UI version your mod was written and
tested against. It never gates anything — your view still loads, your schema
still registers — but when the running OSF UI is older, the Mods surface
shows the "needs update" badge with your mod named in the tooltip, so the
user learns OSF UI is what needs updating. The running host's version arrives
as `runtime.ready`'s `version` if your code must branch on it:

```js
const info = await osfui.ready;
console.log(`running OSF UI ${info.version}`);
```

The protocol version is **1.0**, emitted as `bridgeVersion` — informational
(logs, bug reports), distinct from the plugin `version`. From 1.0 the
contract is stable: additive changes bump the minor version; anything that
would break a shipped view bumps the major. The constant lives in
`src/core/Version.h` (`kBridgeProtocolVersion`); the schemas, `.d.ts`, and
the shared helper are kept in lockstep with it (CI greps the docs against
the constant).
