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
> bridge protocol is at version **2.0 — stable**; additive changes bump the
> minor version, breaking changes bump the major. 2.0 is such a break with
> 1.x: the whole API is four verbs, routing metadata travels beside the
> payload, and the handshake is started by the page. A view written for 1.x still
> loads, but every helper member it calls was removed — so declare
> `targetVersion` (§2, §7) and the Mods surface tells the player which mod
> needs updating instead of showing them a blank panel. `bridgeVersion` is
> informational.

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
  `view`/`views`, `menu.open`, the `osfui/views` state key, `RegisterView`. The
  slash mirrors the folder path; a dotted join would be ambiguous because mod
  ids already contain a dot.

Ids that fail the grammar are rejected at load, with an ERROR in `OSF UI.log`
naming the file and the rule. The same mod id names your settings schema
(`settings/<modId>.json`), your values file, your view namespace folder, the
first half of every state key you publish (`<modId>/<key>`), and the prefix of
your native plugin's endpoints (`<modId>.<name>`) — one identity across every
surface.

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
view, use its qualified id `<modId>/<viewName>` as a `menu.open` target (it
loads on demand). A HUD may declare `openOnStart: true` as its automatic-start
DEFAULT — whether it actually starts with the game is the player's per-HUD
choice in Mod Settings. A native plugin may use `RegisterView` to validate its
shipped view explicitly; ordinary registered views still load on first open.

Views load at `https://osfui.local/<modId>/<viewName>/<entry>`. WebView2 maps
`osfui.local` to the shared views root with
`SetVirtualHostNameToFolderMapping`; it exposes no other local filesystem path.
Keep assets in your own folder. The supported cross-folder contract is the
shared UI kit at `views/shared/osfui.css` / `osfui.js`, linked as
`../../shared/osfui.css`.

Module scripts, dynamic `import()`, and same-origin `fetch()` work under the
WebView2 origin. The built-in views still ship a single classic `main.js` bundle
with stable filenames because those bytes and paths are part of their published
artifact contract; third-party views are not required to copy that build shape.
Remote requests are blocked by the host. Views must keep dependencies and assets
local; `permissions.network` is force-disabled. The built-in bug reporter is a
narrow native exception with a host-owned HTTPS endpoint and cannot be invoked
by third-party views. See `security-model.md`.

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
  subtree with `osfui.theme.applyAccent(el, "#e6904a")` (from
  `shared/osfui.js`), which derives the kit's linked accent set
  (`--osf-accent`, `-hover`, `-strong`, `-quiet`). A missing or invalid value
  clears the whole derived set rather than leaving half of it behind.

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

**Focus and input.** An input-capturing menu gives the WebView real OS focus
for its full visible session. Physical keyboard and IME input therefore work
natively, and Windows schedules the interactive browser as foreground work.
The host captures and forwards mouse input directly. Because Starfield's
Windows.Gaming.Input feed pauses while another process is focused, the runtime
polls an XInput-compatible controller for that interval and preserves the same
default and raw mappings described above. HUD-only views never capture input:
Starfield remains focused and their capture cadence is capped at 60 Hz to bound
GPU/copy pressure during gameplay.

### Localization

Write normal English in your manifest and page. Community translation mods
ship `SFSE/Plugins/OSFUI/l10n/<modId>_<locale>.json`; you don't need an
English catalog or manual string keys. Manifest metadata is addressed as
`views.<viewName>.title` and `views.<viewName>.description` automatically.

For custom page chrome, use a stable address plus inline English:

```js
heading.textContent = osfui.i18n.t("views.myhud.heading", "Ship status");
count.textContent = osfui.i18n.t("views.myhud.itemCount", "{count} items", { count: items.length });
```

Static markup can be translated without page code:

```html
<h1 data-i18n="views.myhud.heading">Ship status</h1>
<input placeholder="Search" data-i18n-placeholder="views.myhud.search">
```

Your catalog is a state key (`osfui/i18n`, §3) that the helper consumes for
you: it arrives with the boot replay, sets `document.documentElement.lang`,
reapplies `data-i18n*` on every live language change, and resolves
`osfui.i18n.ready` for code that must wait for the first catalog. Because it
is state, a page reload gets it again automatically — there is no "request my
catalog on ready" step. Lookup falls back per string: exact locale, base
locale, an optional English override, then the inline English.

---

## 2. `manifest.json` reference

```jsonc
{
  "id": "myhud",            // required; must equal the view folder name. The runtime id is the qualified "<modId>/myhud", derived from the path
  "title": "My HUD",        // optional, defaults to the qualified id
  "description": "",        // optional; one-line blurb shown in catalogs (the osfui/views state key, the Mods surface)
  "accent": "#e6904a",      // optional; colors platform chrome such as the first-load handoff
  "kind": "menu",           // optional, default "menu"; "menu" = modal overlay, "hud" = passive overlay over gameplay. Unknown values fall back to "menu"
  "capturesInput": true,    // optional, default true; MENU-ONLY — freeze the game and route input into the page while this is the top open menu. Forced false for HUDs
  "pausesGame": true,       // optional, default true; MENU-ONLY — pause the simulation while this is the top open menu. Forced false for HUDs
  "openOnStart": false,     // optional, default false; menu: open at load, HUD: show at load
  "order": 0,               // optional, default 0; HUD-ONLY paint order among HUDs (clamped 0..999, higher on top). Ignored for menus
  "hub": true,              // optional, default true; false = hidden utility view — loads and works, but isn't advertised in catalogs (name predates the Mods surface)
  "debugOnly": false,       // optional, default false; keep out of the mod menu list unless the user enables Debug mode (OSF UI → Diagnostics). Still loads and openable by id; intended for built-in developer tools
  "readySignal": true,      // optional, default false; wait for osfui.markReady() before first reveal (requires nativeBridge)
  "targetVersion": "2.0.0", // optional; the OSF UI version this view is authored against — advisory, never gates loading (see note below)
  "entry": "index.html",    // optional, default "index.html"; must stay inside the folder
  "width": 1600,            // optional, default 1600; clamped to 1..16384 — logical (authoring) size
  "height": 900,            // optional, default 900;  clamped to 1..16384 — logical (authoring) size
  "transparent": true,      // optional, default true; lets the game show through
  "permissions": {          // optional; everything defaults to DENY
    "nativeBridge": true,   // false ⇒ no window.osfui bridge is created at all
    "filesystem": false,    // reserved; no effect yet
    "network": false        // reserved; forced off with a warning if set true
  }
}
```

Notes:
- Unknown manifest keys are ignored, so a manifest written for a newer OSF UI
  parses leniently (devMode logs them at INFO). An optional
  `"manifestVersion"` integer is accepted but not required; the nested folder
  layout itself identifies the format.
- `targetVersion` declares the OSF UI version your view was authored against
  (`"<major>[.<minor>[.<patch>]]"`, e.g. `"2.0.0"`). It is advisory: the view
  always loads and does what it can. Two things read it. When the running OSF
  UI is *older* than the target, a warning is written to `OSF UI.log` and the
  Mods surface shows a "needs update" badge next to the OSF UI version number,
  with your mod named in the tooltip — so the user learns OSF UI is what needs
  updating, instead of blaming your mod for missing features. When the target
  is *older than 2.0*, the view is flagged with a `compat.legacy-view` health
  card instead: a 1.x view loads and then renders nothing, because every helper
  member it calls was removed, and a legible card beats a blank panel. A
  malformed value is ignored with a warning; an *undeclared* target is never
  flagged either way (undeclared and unparsable are indistinguishable after
  parsing, and guessing would badge everyone).
- `width`/`height` set the page's logical size; author against it. When the
  `d3d12` compositor is active, the runtime resizes the view to match the
  screen aspect (height capped at 1440) with a matching device scale
  (`outputHeight / height`), so the page always lays out at its logical
  height and CSS pixels scale up to output pixels. At 1440p, a 720-tall
  manifest gets a 2.0 device scale and a CSS viewport 720 px tall — type
  sized for 720p stays that size on screen instead of shrinking. Width still
  varies with the screen's aspect ratio (about 1720 CSS px wide on a 21:9
  display), so write width-responsive CSS. The versioned layout guarantee is
  that your manifest's logical height is fixed; width is not.
- `kind` picks the surface: a `"menu"` may capture input and become the
  focused view; a `"hud"` is passive (see *Multiple views & layering* below).
  `capturesInput` and `pausesGame` refine a menu only — set `pausesGame:false`
  for a menu that wants the world running underneath — and are forced `false`
  for HUDs whatever the manifest says.
- `permissions.nativeBridge` must be `true` if your page talks to the
  runtime. When it is `false`, `window.osfui` is never injected and your page
  runs purely client-side.
- On a menu's first open, OSF UI keeps the new WebView hidden until its main
  frame has loaded. Loads that take longer than 150 ms show a small in-world
  local-link panel carrying the menu's title, accent, input-capture policy, and
  pause policy; already-warm opens remain immediate. A failed load stays on
  that panel with retry/cancel controls instead of revealing a blank surface.
- Set `readySignal:true` when DOM load is too early — for example, when the
  page needs its first state replay before it has anything meaningful to paint.
  After rendering that state, call `osfui.markReady()` once. This field
  requires `permissions.nativeBridge:true`; without it the runtime logs a
  warning and falls back to load completion. If a loaded page never signals,
  the handoff offers retry after 15 seconds so it cannot strand the player.
- A manifest that fails validation (`id` not matching the folder name, an
  `entry` escaping the folder, a folder name violating the id grammar) is
  skipped with an error in `OSF UI.log`. The owning mod id is taken from the
  `views/<modId>/` folder, so a view always groups onto its own mod's page
  automatically.

### Multiple views & layering

Several views can be hosted and composited at once, and there is no central
list to maintain: any valid drop-in folder under `views/<modId>/<viewName>/`
is discovered at boot (deterministically, in id order) and loaded the first
time it is opened through `menu.open`, Papyrus
`OSFUI.OpenMenu("<modId>/<viewName>")`, or the C ABI's
`RequestMenu("<modId>/<viewName>", true)`. This is enough for a Papyrus-only
mod: no companion SFSE plugin is required. Never edit the user's `config.json`
when shipping a view — since `configVersion` 2 it carries no view lists at all.

Whether a HUD starts with the game is player policy: each eligible HUD row in
Mod Settings has a "Start automatically" switch, persisted outside shipped mod
files and applied at the next launch. The HUD manifest's `openOnStart: true`
only sets the default for players who have not chosen. Hidden utility views
(`hub: false`) are not eligible — a surface the player cannot see in the
catalog may not silently run in the background — and `debugOnly` views qualify
only while Debug mode is on. Discovered menus never auto-start; a native
plugin's explicit `RegisterView("<modId>/<viewName>")` still honors
`openOnStart` as plugin opt-in. See
[native-plugin-api.md](native-plugin-api.md) §8c.

The platform pins its own core surfaces (the first-load handoff and the Mods
surface) warm: precreated, prepainted, never reclaimed.

A closed view keeps its document initially. After about 90 seconds of hidden
game time, OSF UI asks WebView2 to suspend it, pausing JavaScript timers and
animation until activity resumes it. A non-pinned view is destroyed and
returned to the discovered state after about 25 hidden minutes — or earlier
when more than four closed views sit hidden (least recently used first; open
surfaces, e.g. a HUD beneath a pausing menu, never count). Reopening a
reclaimed view creates a fresh document, which greets the bridge and is
replayed all of its state (§3) — so an idle reclaim is invisible to a
correctly written view. Treat `ui.visibility` as the visit boundary and do not
rely on hidden timers for required work.

- Layering is set by the menu/HUD framework, not the array order: every HUD
  composites beneath every open menu. HUDs order among themselves by their
  manifest `order` (higher on top, clamped 0..999); open menus stack in the
  order they were opened, top menu on top. An open menu therefore always sits
  above any HUD, whatever the HUD's `order`.
- The focus model is a versioned guarantee. Input goes to exactly one view:
  the top open menu (the stack holds a single menu, so opening a menu replaces
  and focuses it). A `"kind": "hud"` view is passive: it is never focused and
  never receives input, even when it is the top layer. There is no
  user-facing focus-cycle key.
- Each bridge-enabled view (`nativeBridge: true`) has its own bridge.
  Messages are attributed to their source view and replies route back to it,
  so several views can talk to native independently; even a passive HUD can
  receive state and events and post messages.
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

### The whole model is four verbs

Pick the verb by *semantics*, not by transport. There is nothing else, and
there are no aliases — the 1.x surface had two spellings for half of these,
and every one of them was a decision an author had to make twice.

| verb | direction | contract | what happens on F5 |
|---|---|---|---|
| `osfui.send(name, payload?)` | web → backend | One-way. Returns whether the message could be **posted locally** — never a remote outcome. | Nothing. It went with the document. |
| `osfui.request(name, payload?, opts?)` | web → backend | Settles **exactly once**: the reply payload, a typed error, or a timeout. | Pending requests die with the document. Nobody is left holding a promise. |
| `osfui.on(event, fn)` | backend → web | A one-shot happening. Delivered at most once, **never replayed**. | Events you were not open for stay missed — by design. |
| `osfui.state.on(key, fn)` | backend → web | A named value, latest-wins, **complete per key** (never a delta). **Always replayed.** | Replayed automatically to the fresh document, before any event. This is the point. |

The events/state split is the load-bearing one. Replaying an event on reload
re-fires its effect ("you took a hit" three times because the page reloaded
three times); *not* replaying state on reload is the blank HUD. No single
primitive can serve both, which is why 1.x ended up growing both `data.push`
and `data.state` and a convention ("fire a `ready` action so the script
re-pushes") for gluing them together.

**The rule of thumb:** if the backend knows when the value changes, publish
state; if only the view knows when it needs the answer, make a request. And
the corollary that matters most: **a correctly written view has zero lifecycle
code.** If you catch yourself writing "on ready, re-request my data", the
thing you want is state, and the fix belongs in the backend.

### The handshake is page-initiated, and it is the only boot path

```
document loads
  └─ helper sends  { kind: "send", name: "osfui.hello" }
       └─ host answers  ready   (RuntimeInfo)
            └─ then every current state value: the platform keys, then your mod's
                 └─ then the event gate opens and queued events flush
```

First open, a raw F5, a dev hot-reload, a crash-recovery reload, and a view
recreated after an idle reclaim are all *the same sequence*. The helper sends
the hello for you the moment it loads; you never write it.

Because the document initiates, the host never has to guess whether a greeting
was consumed — which is why an F5 the runtime never even hears about still
works. Two ordering guarantees fall out of it, and you can rely on both:

1. `ready` precedes all state for that document.
2. All replayed state precedes the first event that document sees.

Events raised while a view has been created but has not yet greeted are held
in a bounded per-view queue (64, dropping the *oldest* — the newest happenings
are the ones still worth delivering) and flushed right after the replay. State
addressed to an ungreeted document is dropped instead of queued: the replay
carries every current value anyway, and queueing would risk delivering a stale
value after a newer one.

`osfui.ready` resolves with the `ready` payload:

```js
const info = await osfui.ready;
// { game: "Starfield", plugin: "OSF UI", version: "2.0.0",
//   bridgeVersion: "2.0", view: "yourname.mymod/panel", mod: "yourname.mymod" }
```

`version` is the running OSF UI — the reference point for `targetVersion`
(§7). `view` and `mod` tell the document who it is, so a page can build its
own state keys without hardcoding its id. In a plain browser (no bridge)
`osfui.ready` **rejects** with code `no-bridge` rather than hanging forever.

### Envelopes

Every message in both directions is JSON text. Routing metadata (`kind`,
`name`, `id`, `mod`, `key`) sits **beside** an opaque payload, never inside
it — in 1.x the endpoint name lived *in* the payload, which meant a payload
field could override routing.

```jsonc
// web → native
{ "kind": "send",    "name": "close",     "payload": {} }
{ "kind": "request", "name": "menu.open", "id": "q7", "payload": { "view": "yourname.mymod/panel" } }

// native → web
{ "kind": "ready", "payload": { "game": "…", "plugin": "…", "version": "…", "bridgeVersion": "…", "view": "…", "mod": "…" } }
{ "kind": "state", "mod": "yourname.mymod", "key": "credits", "value": 12500 }
{ "kind": "event", "name": "ui.hotkey", "payload": { "mod": "yourname.mymod", "key": "toggleHud" } }
{ "kind": "reply", "id": "q7", "payload": {} }
{ "kind": "error", "id": "q7", "payload": { "code": "unknown-view", "message": "view was not discovered" } }
```

- `id` is **required** on a request and **forbidden** on a send. A missing,
  non-string, empty or over-64-character id is a hard `invalid-request`. (1.x
  silently demoted a bad id to fire-and-forget, which turned a client bug into
  a request that never settled.)
- A present `payload` must be an object. A non-object payload is an
  `invalid-request`, not something to coerce.
- Endpoint kind is enforced structurally. A `request()` naming a send endpoint
  is rejected with `wrong-endpoint-kind`; a `send()` naming a request endpoint
  is **dropped** — executing a mutation whose kind the caller got wrong invites
  worse bugs — but never *silently*: it comes back to your console (see
  *Debugging*).
- Malformed messages are rejected and logged, never fatal. Don't flood the
  bridge — it shares the game thread — and note that one view may have at most
  64 requests in flight before the next one is rejected with
  `request-capacity`.

### The helper surface

This is all of it. It is deliberately thin, because it is the contract:

```js
osfui.available                       // PROPERTY (not a call): false = plain browser
const info = await osfui.ready;       // RuntimeInfo; rejects "no-bridge" standalone

osfui.send("close");                                            // → boolean ("posted")
const reply = await osfui.request("menu.open", { view: id });   // → the reply PAYLOAD
const offEvent = osfui.on("ui.hotkey", (p) => { … });           // → unsubscribe
const offState = osfui.state.on("osfui/settings", render);      // → unsubscribe
const now = osfui.state.get("yourname.mymod/credits");          // latest value, or undefined

osfui.markReady();                                  // manifests with readySignal:true
osfui.papyrus.send("equip", formId, 1);             // one-way, to your own mod's script
const price = await osfui.papyrus.request("price", formId);

await osfui.i18n.ready;                             // first catalog has arrived
osfui.i18n.locale;                                  // PROPERTY, e.g. "de"
osfui.i18n.t("views.myhud.heading", "Ship status");
osfui.i18n.localize(root);                          // apply data-i18n* below a root
osfui.theme.applyAccent(el, "#e6904a");             // never touches the wire
```

Two things to know about `request()`. It resolves with the **reply payload**,
not an envelope — correlation ids are private. And it rejects with an `Error`
carrying a stable machine `.code`, so `catch (err) { if (err.code === … ) }` is
the intended shape. The client timer defaults to 10 000 ms; `{ timeoutMs: 0 }`
disables *only* the client timer, and the host still answers `no-response` at
its own 30 s deadline, so a request can no longer hang forever.

`state.on()` replays the current cached value **synchronously** at subscribe
time if one has already arrived, then fires on every change. Subscribing is a
read; a view that has to ask "has it arrived yet?" is the bug this verb exists
to delete.

State keys are `"<modId>/<key>"` — the owning mod, then the name the backend
published. Platform keys are therefore `osfui/…`, and your own are
`yourname.mymod/…`. Keys are matched case-insensitively on both halves, because
a Papyrus key arrives through `BSFixedString` interning, which hands back the
first casing the process saw rather than what your script spelled.

> **Gone from 1.x**, and gone loudly (`not a function`): `osfui.emit`,
> `osfui.call`, `osfui.action`, `osfui.viewReady`, `osfui.data.*`, top-level
> `osfui.t` / `localize` / `locale()` / `i18nReady` / `applyAccent`, and
> `available()` as a *call*. The one break that fails **silently** is
> `request()`: it used to resolve the whole envelope and now resolves the
> payload, so `reply.payload.x` becomes `reply.x`.

Under the helper sit two primitives (all the helper itself uses):

```js
window.osfui.postMessage(jsonString);          // web → native: one JSON message
window.osfui.onMessage = (jsonString) => { };  // native → web: OWNED BY THE HELPER
```

With `shared/osfui.js` loaded, never assign `onMessage` yourself — subscribe
with `osfui.on()` / `osfui.state.on()` instead. (Messages that arrive before
anything is assigned to `onMessage` are queued by the injected shim and flushed
on assignment, which is why loading the helper in `<head>` or before your own
script is enough.)

### Sends (web → native, one-way)

Anything not listed is dropped and surfaced as `unknown-endpoint`.

| name | payload | effect |
|---|---|---|
| `osfui.hello` | — | greet the bridge. **The helper sends this for you** on every document; you never write it |
| `close` | — | close the calling surface (closing the last open menu hides the overlay; a coexisting live HUD stays up) |
| `setVisible` | `visible: bool` | open/close the calling surface |
| `view.ready` | — | declare that this page has meaningful content ready for its first reveal; only for manifests with `readySignal:true`. Sugar: `osfui.markReady()` |
| `log` | `text: string` | write to `OSF UI.log` (truncated to 512 chars) |
| `osfui.gamepadRaw` | `raw: bool` | *(experimental — exempt from the stability guarantee until stabilized)* take over gamepad handling: suppress the default nav mapping and consume raw `ui.gamepad` events yourself. Cleared whenever your document greets the bridge, i.e. on every (re)load — re-assert it from ordinary boot code, not from a reload handler; there is no reload handler |
| `osfui.handleBack` | `handle: bool` | own the back action. While your menu is ACTIVE, Esc / gamepad B are delivered to your page as a synthetic Escape keydown/keyup instead of closing the top menu — handle it and decide: navigate (`menu.open`), dismiss an inner panel, or `send("close")`. Same per-document lifetime as `osfui.gamepadRaw`. The overlay toggle key always closes natively, so a page that stops responding cannot strand the player |
| `papyrus.send` | `name: string`, `args?: (string\|number\|boolean)[]` | one-way message to the OWNING mod's Papyrus listener, delivered as `OnOSFUIViewAction(name, args)`. The target mod is derived from the calling view's id — the payload cannot spoof it. Sugar: `osfui.papyrus.send(name, ...args)`. See [authoring-dynamic-data.md](authoring-dynamic-data.md) |
| `osfui.handoffRetry` | — | *(platform-private)* retry a stalled first-load handoff; only the handoff surface may call it |

### Requests (web → native, settles exactly once)

| name | payload | resolves | rejects with |
|---|---|---|---|
| `menu.open` | `view?: string` | `{}` | `unknown-view` |
| `menu.close` | `view?: string` | `{}` | `unknown-view` |
| `setViewHidden` | `view?: string`, `hidden: bool` | `{}` | `unknown-view` |
| `ping` | — | `{}` | — |
| `game.get` | — | `{ calendar: { available, day, month, year, hour, daysPassed } }` | — |
| `settings.set` | `mod`, `key`, `value` | `{ mod, key, value }` — `value` is the **post-clamp committed** value, so you can tell clamped from accepted without a re-fetch | `forbidden`, `unknown-setting`, `read-only`, `invalid-value` |
| `settings.reset` | `mod`, `key?` | `{}` — the refreshed registry arrives to everyone as `osfui/settings` state | `forbidden`, `unknown-setting` |
| `settings.captureKey` | `mod`, `key` | `{ armed: true, mod, key }` — settles in MACHINE time | `forbidden`, `capture-busy`, `not-rebindable` |
| `osfui.openModPage` | — | `{}` | `shell-failed` |
| `osfui.openLogFolder` | — | `{}` | `no-log-folder`, `shell-failed` |
| `osfui.setViewAutoStart` | `view`, `enabled` | `{}` | *(platform-private)* `forbidden`, `invalid-payload`, `unknown-view`, `not-configurable`, `persistence-failed` |
| `osfui.openReportIssue` | `issueNumber: number` | `{}` | *(platform-private)* `forbidden`, `invalid-issue`, `shell-failed` |
| `diagnostics.reportStatus` | — | `{ enabled, logs, retentionDays }` | *(platform-private)* `forbidden` |
| `diagnostics.submitReport` | `title`, `description`, `reproduction?` | `{ reportId?, issueNumber? }` | *(platform-private)* `forbidden`, `reporting-disabled`, `invalid-report`, `report-busy`, `consent-declined`, upload codes |
| `papyrus.request` | `name`, `args?` | `{ value }` — the sugar unwraps it to `value` | `invalid-request`, `papyrus-unavailable`, `papyrus-timeout`, or the script's own `RejectViewRequest` code |

Notes on the two that surprise people:

- **`menu.open` resolves "accepted and queued".** The open itself lands on the
  next tick, through the same snapshot/load/pump path as the native
  `RequestMenu`. That is all a caller can act on; the alternative would be a
  reply that waits an arbitrary number of frames on a page load.
- **`settings.captureKey` settles immediately, and the key arrives later.**
  It resolves the moment native capture is *armed*; the key the player actually
  presses comes back as the `settings.captured` **event**, however many seconds
  later. A request left pending on a human fights the client timeout and makes
  "waiting for you" indistinguishable from "the backend died". Requests settle
  in machine time; human-time outcomes are events. (There is consequently no
  `timeoutMs: 0` usage left in 2.0.)

> There is intentionally no "call any native function" escape hatch. New
> endpoints come from native code only: either a handler in the OSF UI
> runtime, or a separate SFSE plugin registering its own through the native
> bridge API ([native-plugin-api.md](native-plugin-api.md)). Plugin endpoints
> must be shaped `<author>.<modname>.<name>` (two dots minimum; the leading
> mod id follows the §0 grammar), so no mod can register any of the platform
> names above, and no registry is needed to keep the two namespaces apart.

### State keys (`osfui.state.on(key, fn)`)

Every one of these replays to every fresh document. Nothing here is requested,
and nothing is re-requested after a reload.

| key | value | when it changes |
|---|---|---|
| `osfui/settings` | `{ mods: [{ id, title, schema, values, shadowed?, targetVersion? }], vanillaKeys?, loadErrors? }` | the registry SHAPE changes (a schema registers, hot-reloads or goes away) or a whole-mod reset lands. Individual commits arrive as the `settings.changed` event. `mod` may be the reserved id `@game` (the game's own bindings — display `title`). `loadErrors` names settings files that failed to load, so a surface can say so instead of a mod silently vanishing |
| `osfui/views` | `{ views: [{ id, title, description, mod, kind, interactive, hub, targetVersion, open, focused, loadState, autoStart, autoStartMutable, pinned }] }` | any open/close/focus/load-state change. `loadState` is `"unloaded"` (discovered on disk, never loaded — still listed so a launcher can offer it) \| `"loading"` \| `"loaded"` \| `"failed"`. Respect `hub:false` — don't list those |
| `osfui/diagnostics` | `{ system, issues: [{ id, code, severity, status, source, subject, context, occurrences, firstAt, lastAt, resolvedAt? }] }` | the session health registry changes. Each issue carries a stable machine `code` — map it to your own copy, the payload never contains player-facing prose. Powers the Mods surface's System Health destination; a normal content view rarely needs it |
| `osfui/i18n` | `{ mod, locale, strings }` | language change or a devMode catalog reload. **Per view**: the value is your own mod's catalog. The helper consumes this for you — use `osfui.i18n.t()` |
| `osfui/handoff` | `{ target, mod, title, accent, phase, retry }` | *(platform-private)* the first-load handoff surface's own state |
| `<yourModId>/<key>` | whatever your backend published | your Papyrus script's `OSFUI.SetView*` or your plugin's `SetViewState`. See [authoring-dynamic-data.md](authoring-dynamic-data.md) |

### Events (`osfui.on(name, fn)`)

| name | payload | when |
|---|---|---|
| `settings.changed` | `{ mod, key, value, conflicts? }` | one committed value changed — whoever wrote it (settings menu, preset, reset, Papyrus, native). `value` is post-validation and therefore authoritative. On a `key`-typed setting, `conflicts` is the recomputed collision list (always present, `[]` = none) so badges update without re-reading the registry. You receive changes for **all** mods — filter on `mod` |
| `settings.persisted` | `{ mod }` | that mod's values file actually hit disk (writes are coalesced ~500 ms). Drives a "Saved" indicator |
| `settings.captured` | `{ mod, key, name, cancelled, conflicts? }` | the outcome of a `settings.captureKey`: the captured key `name` (an OSF UI key name), or `cancelled:true` (Escape / unbindable — keep the old binding). `conflicts` lists collisions this bind *would* create; warn live, never block. The view then sends a normal `settings.set` with `name` |
| `ui.hotkey` | `{ mod, key }` | the physical key bound to that `key`-typed setting was pressed in-game. Delivered to every bridged view — filter on `mod`. Suppressed while the overlay captures input or a rebind is armed |
| `ui.visibility` | `{ visible, reason? }` | this view was shown/hidden as the overlay's focused menu (edge-triggered). Fires on overlay open/close AND on a `menu.open` view switch while the overlay stays up: the outgoing view gets `visible:false`, the incoming one `visible:true`. `reason` is `"overlay"` (the overlay itself) or `"focus"` (only the focused menu changed). Treat any `visible:false` as a real hide |
| `ui.gamepad` | `{ kind:"button", button:{id, down} }` \| `{ kind:"stick", axes:{lx, ly, rx, ry} }` | *(experimental)* raw gamepad events to the ACTIVE view while the overlay captures input. The default nav mapping also applies unless you asserted `osfui.gamepadRaw` |
| `<modId>.<name>` | `{ args: string[] }` | a one-shot happening announced by the owning mod's Papyrus (`OSFUI.SendViewEvent`) or its native plugin (`SendToWeb`). Never cached, never replayed — see [authoring-dynamic-data.md](authoring-dynamic-data.md) |
| `osfui.debug.error` | `{ code, message, detail? }` | **devMode only**, and the helper intercepts it: it prints to your console and does *not* deliver it to `on()` handlers. See below |

Ignore event names you don't know (and never `eval` them), including ones a
future runtime version may add.

### Errors and how you find out about them

Every `request()` rejection carries a stable `code`. The layers are kept
distinguishable on purpose, because they point at different culprits:

| code | meaning |
|---|---|
| `no-bridge` | local and immediate — a plain browser, or `nativeBridge:false` |
| `timeout` | the CLIENT timer gave up (default 10 s) |
| `no-response` | the BACKEND missed the host-side 30 s deadline |
| `wrong-endpoint-kind` | you used `request()` on a send endpoint (or `send()` on a request one) |
| `unknown-endpoint` | no such endpoint |
| `invalid-request` | malformed envelope: bad kind, empty name, bad/missing id, non-object payload |
| `request-capacity` | this view already has 64 requests in flight |
| anything else | the handler's own code (`unknown-view`, `capture-busy`, a Papyrus script's `RejectViewRequest` code, …) |

**Debugging is F12 Chromium DevTools** (devMode only). OSF UI does not build
its own inspector; instead 2.0 makes sure everything an author can get wrong
actually *reaches* the page console, where DevTools shows it with full object
inspection — and, because devMode forwards console output over the host pipe,
in `OSF UI.log` at the same time.

- The helper prints every request rejection, every `no-bridge`, and every
  client timeout with an `[osfui]` prefix, so an unhandled promise rejection
  is preceded by a named, inspectable error instead of being opaque.
- Mistakes the page would otherwise never hear about — a `send` dropped for
  naming a request endpoint, a send to an unknown endpoint, a backend that
  missed its deadline — come back to the offending view as the dev-only
  `osfui.debug.error` event, and the helper prints those the same way.
- In release builds there is no debug channel, so *repetition* is the signal:
  a view that keeps getting the protocol wrong raises a `view.protocol-misuse`
  health card on the Mods surface. A one-off stays out of the player's face.
- For "what is actually crossing the bridge", set
  `localStorage["osfui:trace"] = "1"` and reload. The helper then logs every
  envelope in both directions via `console.debug`, including the settlement
  latency of each request. This is also how you answer "why is my HUD blank":
  with tracing on, every replayed `state` envelope is visible at document boot,
  so either the key arrives (view bug) or it doesn't (backend bug).

### Minimal example

```html
<script src="../../shared/osfui.js"></script>
<script>
"use strict";

// State: no request, no re-request after a reload. The handler runs
// immediately with the current value once it has arrived, and again on
// every change — including on the fresh document after an F5.
osfui.state.on("osfui/settings", (data) => {
  const mine = data.mods.find((m) => m.id === "yourname.mymod");
  if (mine) applyAll(mine.values);
});

// Events: one-shot happenings only.
osfui.on("settings.changed", (p) => {
  if (p.mod === "yourname.mymod") applySetting(p.key, p.value);
});
osfui.on("ui.visibility", (p) => { if (!p.visible) stopAnimations(); });

// Sends: nothing to await.
document.getElementById("close").onclick = () => osfui.send("close");

// Requests: when the outcome matters.
async function openAlmanac() {
  try {
    await osfui.request("menu.open", { view: "yourname.mymod/almanac" });
  } catch (err) {
    console.warn("open failed:", err.code);  // e.g. "unknown-view"
  }
}

// `ready` is for identity and version branching — NOT for fetching data.
osfui.ready.then((info) => {
  document.title = `${info.plugin} ${info.version}`;
});
</script>
```

See [`frontend/src/views/osfui/settings/`](../frontend/src/views/osfui/settings/)
for a complete, commented example — that is the *source* of the built-in Mods
surface. The copy under `build/frontend/views/osfui/settings/` is disposable
build output; inspect it if you want the exact shipped bytes, but never edit it.

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
| `key` | press-to-bind button | key-name string (≤16 chars), non-empty unless the setting sets `"allowUnbound": true` — then `""` is the deliberate unbound state (no hotkey dispatch, no conflict badges, and the UI adds an unbind ×). Framework-managed: capture is armed via `settings.captureKey` and grabbed in the native input layer, so pressing the current toggle key rebinds instead of closing the overlay. Every `key`-typed setting of every mod is rebindable and dispatches via the HotkeyService (`ui.hotkey` / `SubscribeHotkey`); optional immutable `onPress: {script,function}` metadata additionally queues that GLOBAL Papyrus callback after the normal channels |

This is the frozen base type set. There is no `color` type; use
`type:"string"` + `widget:"color"`. Evolution is a base type plus a `widget`
hint and attributes; a genuinely new base type ships in a new OSF UI version
that your schema names via `targetVersion`.

**Forward compatibility.** A host that predates one of your setting types
renders that row read-only ("needs a newer OSF UI"), serves the schema
`default` to consumers, and preserves the user's saved value untouched — it
round-trips through every rewrite, and a newer host picks it back up. If
your schema uses anything newer than the OSF UI you tested against, declare
that version so older hosts tell the user to update:

```jsonc
{ "id": "yourname.mymod", "targetVersion": "2.0.0" }
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
server-side, and the `settings.set` request **rejects** with the code
(`unknown-setting`, `read-only`, `invalid-value`). Untrusted JS cannot write
arbitrary keys, out-of-range values, or to any path but its own settings file:
a view may only write **its own mod's** settings (`forbidden` otherwise) —
only the built-in Mods surface and keybinds board may name a foreign mod,
because editing other mods' settings is their entire purpose.

### Reacting to changes from your view

The registry is the `osfui/settings` state key, and committed values are the
`settings.changed` event. That pairing is the whole subscription model — there
is no read call, and nothing to redo after a reload:

```js
osfui.state.on("osfui/settings", (data) => {
  const mine = data.mods.find((m) => m.id === "yourname.mymod");
  if (mine) applyAll(mine.values);            // replayed on every document
});

osfui.on("settings.changed", (p) => {
  if (p.mod === "yourname.mymod") applySetting(p.key, p.value);
});
```

The event's `value` is post-validation and therefore authoritative; on
`key`-typed settings its `conflicts` list is recomputed, so badges update
without re-reading the registry. You receive changes for all mods — filter on
`p.mod`. A mod's HUD therefore reacts live to its own settings with zero
polling and zero native code.

(In 1.x this took a `settings.get` whose *real* job was to subscribe you, and
which every view had to remember to re-send after a reload. That invisible
side effect is exactly what the `state` verb replaced.)

### Hotkeys

Every `type:"key"` setting is a live hotkey: when the user presses the bound
key in-game, the runtime raises `ui.hotkey { mod, key }`. So this makes your
HUD toggleable:

```js
let visible = false;
osfui.on("ui.hotkey", async (p) => {
  if (p.mod !== "yourname.mymod" || p.key !== "toggleHud") return;
  visible = !visible;
  await osfui.request(visible ? "menu.open" : "menu.close",
                      { view: "yourname.mymod/hud" });
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
needed: `SubscribeSettings` plus typed getters for values, and
`SubscribeHotkey` for key presses. See
[native-plugin-api.md](native-plugin-api.md) §8a/§8b. In-tree framework knobs
still react through
`Runtime::OnSettingChanged` (e.g. `osfui.toggleKey` live-rebinds the
overlay's open/close key).

---

## 5. Testing locally

For a new view, use the one-command npm workflow described in
[view-toolchain.md](view-toolchain.md):

```bat
npm create osfui@latest my-view
cd my-view
npm run dev
```

It provides the browser harness below plus source presets, Vite HMR, generated
manifests, in-game sync, temporary author mode, validation, and packaging.

### Browser harness

`npm run dev` serves the view at the same `/<modId>/<viewName>/<entry>` URL
shape used in game, generates and validates `manifest.json` from
`osfui.config.ts` or `osfui.config.js`, supplies OSF UI's public
`shared/osfui.js` and `shared/osfui.css`, and installs a mock backend that
speaks the real 2.0 protocol — including the page-initiated handshake, so a
harness reload exercises the same boot path the game does.

The toolbar provides:

- manifest and custom resolutions, scaled down without changing page layout;
- visible/hidden lifecycle edges, locale changes, page reload and a
  transparency checkerboard;
- a bridge traffic inspector — one scannable row per envelope
  (`send · close`, `request · settings.set`, `state · osfui/settings`,
  `hotkey · F9`, `error · unknown-endpoint`) that expands to the raw envelope
  on click, pairs each request with its reply and round-trip time, folds
  repeats into a `×N` counter, and can be filtered or paused.

Saving view source updates the page through Vite HMR. Development responses use
`Cache-Control: no-store`. A document CSP blocks remote resources, and the
bootstrap removes WebRTC, WebTransport and worker constructors to catch
unsupported dependencies. WebSocket stays available only for Vite's loopback HMR
connection; the `osfui check` command rejects authored uses of unsupported
transports.

For repeatable backend data, describe what your backend would do in the
project's mock (`osfui.mock.ts` with `defineMock`, or a plain
`osfui.mock.json`, beside `osfui.config.ts`); the browser reloads when it
changes:

```json
{
  "locale": "en",
  "locales": {
    "de": { "views.myhud.heading": "Schiffsstatus" }
  },
  "state": {
    "credits": 12500,
    "slots": ["Med Pack", "Frag Grenade"]
  },
  "requests": {
    "yourname.mymod.inventory.get": {
      "items": [{ "name": "Med Pack", "count": 4 }]
    },
    "papyrus.calculatePrice": 125
  }
}
```

Each `state` entry is published under **your own mod id** — `credits` above
arrives as the state key `yourname.mymod/credits` — and is replayed right
after `ready`, exactly as the runtime replays it. A `requests` entry answers
that request endpoint (values may be plain JSON or, in a `.ts`/`.js` mock, a
function of the payload); prefix a key with `papyrus.` to answer
`osfui.papyrus.request("…")`. Unconfigured endpoints are not faked: a request
rejects with `mock-unhandled`, and a send to an unknown name surfaces
`unknown-endpoint` — the same way the real host would tell you. A mock that
answers a name your view *sent* one-way is flagged as a mock authoring
mistake, because a send has nothing to settle. Named `scenarios` shallow-overlay
these fields (`?scenario=<name>`); see
[view-toolchain.md](view-toolchain.md). The mock lives at the project root, so
it can never ship with the views.

For a minimal standalone fallback, a view can still detect a missing bridge:

```js
if (!osfui.available) {
  // running in a plain browser — stub or no-op the native calls
}
```

(`shared/osfui.js` installs itself even without a bridge, so `osfui.available`,
`osfui.on()` and `osfui.state.on()` are always safe to touch; `osfui.ready`
and `osfui.request()` reject with code `"no-bridge"`, and the helper logs one
`[osfui]` notice — a plain-browser preview is not an authoring mistake.)

Serve that fallback over `http://` rather than opening it from `file://` so
local testing uses a normal origin like the in-game `https://osfui.local`
mapping.

### Built-in OSF UI views

The built-in views (`osfui/settings`, `osfui/keybinds`) are no longer
openable this way: their shipped `index.html` is generated output. They are
developed through the **same `osfui dev` authoring harness this guide
describes** — the frontend directory is itself an `@osfui/cli` project whose
mock module (`frontend/osfui.mock.ts`) speaks the real protocol:

```bat
npm --prefix frontend run dev
```

See [`frontend/README.md`](../frontend/README.md) for the deep-link URLs and
the locale/fixture/stage switches.

In-game, watch `Documents\My Games\Starfield\SFSE\Logs\OSF UI.log`:
- `MessageBridge: [web] ...` — your `log` sends.
- `MessageBridge: view '<id>' greeted — ready, state replay, events open` — the
  handshake completed for that document.
- `MessageBridge: [content] dropped send to unknown endpoint '...'` /
  `rejected request to unknown endpoint '...'` — you named something that does
  not exist (logged once per name; your page's console gets it every time).
- Set `devMode: true` in `config.json` for verbose renderer and bridge logging.

With `devMode: true` the in-game loop is fast too:
- **Settings schemas hot-reload**: edits to `settings/*.json` are picked up
  within about a second. Values are preserved (a renamed key carries over via
  its `aliases`), an open settings view repaints itself, and deleting the
  file drops the mod. A runtime-registered (DLL) schema is never touched by
  files.
- **Loose view auto-reload**: save HTML, JavaScript, CSS, or a local asset in
  a loaded view and OSF UI reloads it after the file settles (normally within
  half a second). The replacement document greets the bridge itself and is
  replayed all of its state, so a hot-reload is not a special case. Polling and
  MO2 mirror synchronization happen in the background; removed or renamed files
  disappear from the mirror too. It deliberately ignores `manifest.json`
  changes and new view folders, which require a game restart. Built-in views
  still require `npm --prefix frontend run build` because their shipped files
  are generated.
- **DevTools** (`F12`): opens Edge DevTools for the top open menu — the debug
  surface 2.0's error routing is aimed at. It is available only in `devMode`;
  outside authoring sessions the browser capability is disabled.

---

## 6. Checklist for shipping a view

- [ ] `views/<modId>/<viewName>/manifest.json` — folder names pass the id grammar (§0), manifest `id` equals the view folder name, `permissions.nativeBridge` set as needed.
- [ ] Responsive CSS (no hardcoded 1280×720 assumptions; the view is resized to the screen).
- [ ] All assets local and relative (no `..`, no absolute paths, no network) — plus the sanctioned `../../shared/osfui.css` / `../../shared/osfui.js`.
- [ ] Load `shared/osfui.js` before your script. Declare `targetVersion` (`"2.0.0"` or later) — a view that declares a 1.x target is badged as legacy.
- [ ] **No lifecycle code.** Everything the page renders comes from `osfui.state.on()`; nothing is re-requested on load, and nothing depends on `osfui.ready` having fired first.
- [ ] Verbs chosen by semantics: `request()` (and its rejection `code`) where the outcome matters, `send()` where it cannot fail, `on()` only for happenings.
- [ ] (If configurable) a `settings/<modId>.json` schema with sane `default`/`min`/`max`.
- [ ] Verified standalone in a browser, then in-game — including a mid-session F5 (devMode) to prove the page repaints from state alone.

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
  type `window.osfui`, the send/request endpoint whitelists, the platform state
  keys and events, and the settings-schema shapes. Reference it from your
  view's TS project and the bridge is typed globally — no package to install.

### Versioning

Declare what you authored against; degrade gracefully at runtime. One
advisory field, `targetVersion`, appears in both author-facing files (view
manifest §2, settings schema §4): the OSF UI version your mod was written and
tested against. It never gates anything — your view still loads, your schema
still registers — but it is what lets the Mods surface tell the player
something true: "needs update" when the running OSF UI is older than your
target, and a `compat.legacy-view` card when your target predates 2.0. The
running host's version arrives as `ready`'s `version` if your code must branch
on it:

```js
const info = await osfui.ready;
console.log(`running OSF UI ${info.version} (bridge ${info.bridgeVersion})`);
```

The protocol version is **2.0**, emitted as `bridgeVersion` — informational
(logs, bug reports), distinct from the plugin `version`. Additive changes bump
the minor version; anything that would break a shipped view bumps the major,
and 2.0 is such a break: four verbs, envelopes carrying routing beside the
payload, a page-initiated handshake. The constant lives in
`src/core/Version.h` (`kBridgeProtocolVersion`); the schemas, `.d.ts`, and
the shared helper are kept in lockstep with it (CI greps the docs against
the constant).
