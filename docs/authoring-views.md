# Authoring Views & Settings

How to build a UI for OSF UI without touching the C++ runtime. Two data-driven extension points work today:

1. **Views** — an HTML/CSS/JS page rendered as an in-game overlay.
2. **Settings schemas** — typed settings rendered in the built-in `settings` view (MCM-style), persisted and validated natively.

Both are pure content, no recompile: a `views/<modId>/<viewName>/` folder and a `settings/<modId>.json` schema.

The bridge protocol is at version **2.0 — stable**. Additive changes bump the minor, breaking changes the major; 2.0 was such a break with 1.x (four verbs, routing beside the payload, page-initiated handshake). Explicitly pre-2.0 views are refused on open and reported through System Health; there is no compatibility façade. `bridgeVersion` is informational.

> Written with Claude and reviewed against the source. Where it disagrees with the code, the JSON Schemas (§7) or `sdk/osfui.d.ts`, those win — and a bug report about the mismatch is welcome.

## 0. Identifiers

Every public identifier derives from your mod id:

- **Mod id** — `<author>.<modname>`, e.g. `ozooma10.almanac`. Lowercase `[a-z0-9-]` segments, exactly one dot, max 64 chars. The author segment is a handle you already own (Nexus or GitHub username), self-allocated, no registry. Dotless ids are reserved for the platform (`osfui` is the only dotless built-in), so there's no reserved-word list to collide with.
- **View name** — `[a-z0-9-]+`, local to your mod (`planets`).
- **Qualified view id** — `<modId>/<viewName>` (`ozooma10.almanac/planets`), used everywhere views are referenced: `config.json` `view`/`views`, `menu.open`, the `osfui/views` state key, `RegisterView`. The slash mirrors the folder path; a dotted join would be ambiguous, since mod ids already contain a dot.

Ids failing the grammar are rejected at load, with an ERROR in `OSF UI.log` naming the file and rule. The same mod id names your settings schema (`settings/<modId>.json`), your values file, your view namespace folder, the first half of every state key you publish (`<modId>/<key>`), and the prefix of your native plugin's endpoints (`<modId>.<name>`).

Two author prefixes are claimed: `osfui` (platform, reserved, dotless) and `osf` (the OSF family — `osf.animation` and future `osf.*` siblings). Don't publish under someone else's author segment; when two mods collide, whichever loads first wins.

Before authoring, read [security-model.md](security-model.md): your view is untrusted code. No network access, no filesystem access beyond your own folder, no way to call arbitrary native functions.

---

## 1. View package layout

A view is a folder inside your mod's namespace folder under the plugin data dir:

```
SFSE/Plugins/OSFUI/views/<author>.<modname>/<viewname>/
  manifest.json     required — declares the view
  index.html        your entry page (name configurable via manifest "entry")
  style.css         (optional) your styles
  main.js           (optional) your logic
  assets/...        (optional) images/fonts — local only
```

The two-level layout is discovered automatically at load. A mod folder may hold several views, and subfolders without a `manifest.json` are ignored, so shared assets can sit next to your views. Built-in views use the same layout (`views/osfui/settings/`, `views/osfui/keybinds/`). To open your view, use its qualified id as a `menu.open` target — it loads on demand. A HUD may declare `openOnStart: true` as its automatic-start DEFAULT; whether it actually starts is the player's per-HUD choice in Mod Settings. A native plugin may use `RegisterView` to validate its shipped view explicitly; ordinary registered views still load on first open.

Views load at `https://osfui.local/<modId>/<viewName>/<entry>`. WebView2 maps `osfui.local` to the shared views root with `SetVirtualHostNameToFolderMapping` and exposes no other local path. Keep assets in your own folder; the one supported cross-folder contract is the shared UI kit at `views/shared/osfui.css` / `osfui.js`, linked as `../../shared/osfui.css`.

Module scripts, dynamic `import()` and same-origin `fetch()` work under the WebView2 origin. (The built-in views still ship a single classic `main.js` bundle with stable filenames because those bytes and paths are part of their published artifact contract; third-party views needn't copy that build shape.) Remote requests are blocked by the host — keep dependencies and assets local, `permissions.network` is force-disabled. OSF UI has no native diagnostic-upload path. See [security-model.md](security-model.md).

### The shared UI kit

`shared/osfui.css` is the design system every shipped view uses:

- Everything it exports is prefixed: classes `osf-*`, custom properties `--osf-*`. Nothing un-prefixed is part of the contract, so your own names can't collide with a kit update.
- Linking it is opt-in and all-or-nothing. It styles element-level bases (body, headings, `a`, `kbd`, `::selection`, scrollbars, form elements) globally, as any base sheet does. Link it for the native look, or don't and own all your styling; there is no partial mode.
- Theming is a single accent color — no theme classes. A mod's accent is the `accent` value in its schema or manifest. Apply it to any subtree with `osfui.theme.applyAccent(el, "#e6904a")` (from `shared/osfui.js`), which derives the kit's linked accent set (`--osf-accent`, `-hover`, `-strong`, `-quiet`). A missing or invalid value clears the whole derived set rather than leaving half of it behind.

### `padnav.js` is not part of the shared kit

The built-in views ship a third script, `views/osfui/padnav.js`, doing spatial gamepad/focus navigation. It is **private to the `osfui` views** by its own declaration — not published, not versioned, not frozen. So:

- **Don't link it.** It lives under the `osfui/` mod namespace, not `shared/`, so reaching it needs the `..` the sandbox rejects. Only `shared/osfui.css` and `shared/osfui.js` are contract paths.
- **Don't depend on its DOM conventions** (`.row` bands, `.listening`, `[data-nav-modal]`) — an internal arrangement, not an API.

Native gamepad→UI mapping (D-pad and left stick→arrows, A→Enter, B→back, right stick→scroll) reaches *every* view regardless, so basic controller use works without padnav. For richer focus handling, own it in your own script — or take raw events with `osfui.gamepadRaw` (§3). See [`frontend/COMPATIBILITY.md`](../frontend/COMPATIBILITY.md) §3.

**Focus and input.** An input-capturing menu gives the WebView real OS focus for its full visible session, so physical keyboard and IME input work natively and Windows schedules the browser as foreground work. The host captures and forwards mouse input directly. Because Starfield's Windows.Gaming.Input feed pauses while another process is focused, the runtime polls an XInput-compatible controller for that interval, preserving the same default and raw mappings. HUD-only views never capture input: Starfield stays focused and their capture cadence is capped at 60 Hz to bound GPU/copy pressure.

### Localization

Write normal English in your manifest and page. Community translation mods ship `SFSE/Plugins/OSFUI/l10n/<modId>_<locale>.json`; you need no English catalog or manual string keys. Manifest metadata is addressed as `views.<viewName>.title` / `.description` automatically.

For custom page chrome, use a stable address plus inline English:

```js
heading.textContent = osfui.i18n.t("views.myhud.heading", "Ship status");
count.textContent = osfui.i18n.t("views.myhud.itemCount", "{count} items", { count: items.length });
```

Static markup translates without page code:

```html
<h1 data-i18n="views.myhud.heading">Ship status</h1>
<input placeholder="Search" data-i18n-placeholder="views.myhud.search">
```

Your catalog is a state key (`osfui/i18n`, §3) the helper consumes for you: it arrives with the boot replay, sets `document.documentElement.lang`, reapplies `data-i18n*` on every live language change, and resolves `osfui.i18n.ready` for code that must wait for the first catalog. Because it's state, a reload gets it again — there's no "request my catalog on ready" step. Lookup falls back per string: exact locale, base locale, an optional English override, then the inline English.

---

## 2. `manifest.json` reference

```jsonc
{
  "title": "My HUD",        // optional, defaults to the qualified id
  "description": "",        // optional; one-line blurb shown in catalogs (the osfui/views state key, the Mods surface)
  "accent": "#e6904a",      // optional; colors platform chrome such as the first-load handoff
  "kind": "menu",           // optional, default "menu"; "menu" = modal overlay, "hud" = passive overlay over gameplay. Unknown values fall back to "menu"
  "capturesInput": true,    // optional, default true; MENU-ONLY — freeze the game and route input into the page while this is the top open menu. Forced false for HUDs
  "pausesGame": true,       // optional, default true; MENU-ONLY — pause the simulation while this is the top open menu. Forced false for HUDs
  "openOnStart": false,     // optional, default false; menu: open at load, HUD: show at load
  "order": 0,               // optional, default 0; HUD-ONLY paint order among HUDs (clamped 0..999, higher on top). Ignored for menus
  "hub": true,              // optional, default true; false = hidden utility view — loads and works, but isn't advertised in catalogs (name predates the Mods surface)
  "debugOnly": false,       // optional, default false; keep out of the mod menu list unless devMode is on in OSF UI's config.json. Still loads and openable by id; intended for built-in developer tools
  "readySignal": true,      // optional, default false; wait for osfui.markReady() before first reveal (requires nativeBridge)
  "targetVersion": "2.0.0", // optional; the OSF UI version this view is authored against — advisory, never gates loading
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

- Unknown keys are ignored, so a manifest written for a newer OSF UI parses leniently (devMode logs them at INFO). An optional `"manifestVersion"` integer is accepted but not required — the nested folder layout identifies the format.
- **`targetVersion`** is `"<major>[.<minor>[.<patch>]]"`. When the running OSF UI is *older* than the target, a warning goes to `OSF UI.log` and the Mods surface shows a "needs update" badge next to the OSF UI version with your mod named in the tooltip. A declared target older than 2.0 is refused on open and shown as a System Health error. A malformed or undeclared value is treated as current rather than guessing.
- **`width`/`height`** set the page's logical size; author against it. Under the `d3d12` compositor the runtime resizes the view to the screen aspect (height capped at 1440) with a matching device scale (`outputHeight / height`), so the page always lays out at its logical height and CSS pixels scale up. At 1440p a 720-tall manifest gets a 2.0 device scale and a 720 px CSS viewport — type sized for 720p stays that size instead of shrinking. Width still varies with aspect ratio (~1720 CSS px on 21:9), so write width-responsive CSS. The versioned guarantee is that your logical height is fixed; width is not.
- **`kind`** picks the surface: a `"menu"` may capture input and become focused; a `"hud"` is passive (see *Multiple views & layering*). `capturesInput` and `pausesGame` refine a menu only — set `pausesGame:false` for a menu that wants the world running — and are forced `false` for HUDs whatever the manifest says.
- **`permissions.nativeBridge`** must be `true` if your page talks to the runtime. When false, `window.osfui` is never injected and the page runs purely client-side.
- On a menu's first open, OSF UI keeps the WebView hidden until its main frame loads. Loads over 150 ms show a small in-world local-link panel carrying the menu's title, accent, input-capture policy and pause policy; warm opens stay immediate. A failed load stays on that panel with retry/cancel controls rather than revealing a blank surface.
- Set **`readySignal:true`** when DOM load is too early — e.g. the page needs its first state replay before it has anything meaningful to paint. After rendering that state, call `osfui.markReady()` once. Requires `permissions.nativeBridge:true`; without it the runtime warns and falls back to load completion. If a loaded page never signals, the handoff offers retry after 15 seconds so it can't strand the player.
- A manifest failing validation (an `entry` escaping the folder, a folder name violating the id grammar) is skipped with an error in `OSF UI.log`. The owning mod id comes from the `views/<modId>/` folder, so a view always groups onto its own mod's page.

### Multiple views & layering

Several views can be hosted and composited at once, with no central list to maintain: any valid drop-in folder under `views/<modId>/<viewName>/` is discovered at boot (deterministically, in id order) and loaded the first time it's opened through `menu.open`, Papyrus `OSFUI.OpenMenu("<modId>/<viewName>")`, or the C ABI's `RequestMenu("<modId>/<viewName>", true)`. That's enough for a Papyrus-only mod — no companion SFSE plugin required. Never edit the user's `config.json` when shipping a view; since `configVersion` 2 it carries no view lists at all.

Whether a HUD starts with the game is player policy: each eligible HUD row in Mod Settings has a "Start automatically" switch, persisted outside shipped mod files and applied at the next launch. A HUD manifest's `openOnStart: true` only sets the default for players who haven't chosen. Hidden utility views (`hub: false`) aren't eligible — a surface the player can't see in the catalog may not silently run in the background — and `debugOnly` views qualify only while devMode is on. Discovered menus never auto-start; a plugin's explicit `RegisterView` still honors `openOnStart` as plugin opt-in ([native-plugin-api.md](native-plugin-api.md) §8c).

The platform pins its own core surfaces (the first-load handoff and the Mods surface) warm: precreated, prepainted, never reclaimed.

A closed view keeps its document initially. After ~90 seconds of hidden game time, OSF UI asks WebView2 to suspend it, pausing JS timers and animation until activity resumes it. A non-pinned view is destroyed and returned to the discovered state after ~25 hidden minutes — or earlier when more than four closed views sit hidden (least recently used first; open surfaces, e.g. a HUD beneath a pausing menu, never count). Reopening a reclaimed view creates a fresh document, which greets the bridge and is replayed all of its state (§3), so an idle reclaim is invisible to a correctly written view. Treat `ui.visibility` as the visit boundary; don't rely on hidden timers for required work.

- Layering is set by the menu/HUD framework, not array order: every HUD composites beneath every open menu. HUDs order among themselves by manifest `order` (higher on top, clamped 0..999); open menus stack in open order, top menu on top. An open menu always sits above any HUD.
- The focus model is a versioned guarantee. Input goes to exactly one view: the top open menu (the stack holds a single menu, so opening one replaces and focuses it). A `"kind": "hud"` view is passive: never focused, never receives input, even as the top layer. There is no user-facing focus-cycle key.
- Each bridge-enabled view has its own bridge. Messages are attributed to their source view and replies route back to it, so several views talk to native independently; even a passive HUD can receive state and events and post messages.
- Each view is sized to the whole screen — position content with CSS and keep the rest transparent. Layers blend by alpha.

### Mouse & cursor

Don't draw your own pointer. While the overlay captures input the runtime shows the real Windows hardware cursor: zero lag, composited by the display hardware, independent of game framerate. Your page's CSS `cursor` maps to the matching system cursor (`pointer` → hand, `text` → I-beam, the resize variants; `none` hides it; anything exotic falls back to the arrow), so hover feedback works exactly like a browser. A page-drawn `<div>` pointer would trail the real one by the full render pipeline (the shipped views used to do this; it was removed for feeling laggy), and `cursor: none` on `body` would hide the pointer for the whole view.

---

## 3. The bridge — `window.osfui`

When `nativeBridge` is granted, the runtime injects one object before your page scripts run. Use it through the shipped helper, loaded before your own script:

```html
<script src="../../shared/osfui.js"></script>
<script src="main.js"></script>
```

### The whole model is four verbs

Pick the verb by *semantics*, not transport. There is nothing else, and no aliases.

| verb | direction | contract | what happens on F5 |
|---|---|---|---|
| `osfui.send(name, payload?)` | web → backend | One-way. Returns whether the message could be **posted locally** — never a remote outcome. | Nothing. It went with the document. |
| `osfui.request(name, payload?, opts?)` | web → backend | Settles **exactly once**: the reply payload, a typed error, or a timeout. | Pending requests die with the document. |
| `osfui.on(event, fn)` | backend → web | A one-shot happening. Delivered at most once, **never replayed**. | Events you weren't open for stay missed — by design. |
| `osfui.state.on(key, fn)` | backend → web | A named value, latest-wins, **complete per key** (never a delta). **Always replayed.** | Replayed automatically to the fresh document, before any event. |

The events/state split is the load-bearing one. Replaying an event on reload re-fires its effect; *not* replaying state on reload is the blank HUD. No single primitive serves both.

**Rule of thumb:** if the backend knows when the value changes, publish state; if only the view knows when it needs the answer, make a request. Corollary: **a correctly written view has zero lifecycle code.** If you're writing "on ready, re-request my data", the thing you want is state, and the fix belongs in the backend.

### The handshake is page-initiated, and it is the only boot path

```
document loads
  └─ helper sends  { kind: "send", name: "osfui.hello" }
       └─ host answers  ready   (RuntimeInfo)
            └─ then every current state value: the platform keys, then your mod's
                 └─ then the event gate opens and queued events flush
```

First open, a raw F5, a dev hot-reload, a crash-recovery reload and a view recreated after an idle reclaim are all *the same sequence*. The helper sends the hello for you; you never write it. Because the document initiates, the host never has to guess whether a greeting was consumed — which is why an F5 the runtime never hears about still works.

Two orderings you can rely on:

1. `ready` precedes all state for that document.
2. All replayed state precedes the first event that document sees.

Events raised while a view exists but hasn't greeted are held in a bounded per-view queue (64, dropping the *oldest*) and flushed right after the replay. State addressed to an ungreeted document is dropped instead: the replay carries every current value anyway, and queueing would risk delivering a stale value after a newer one.

`osfui.ready` resolves with the `ready` payload:

```js
const info = await osfui.ready;
// { game: "Starfield", plugin: "OSF UI", version: "2.0.0",
//   bridgeVersion: "2.0", view: "yourname.mymod/panel", mod: "yourname.mymod" }
```

`version` is the running OSF UI — the reference point for `targetVersion` (§7). `view` and `mod` tell the document who it is, so a page can build its own state keys without hardcoding its id. In a plain browser (no bridge) `osfui.ready` **rejects** with code `no-bridge` rather than hanging.

### Envelopes

Every message both directions is JSON text. Routing metadata (`kind`, `name`, `id`, `mod`, `key`) sits **beside** an opaque payload, never inside it — in 1.x the endpoint name lived *in* the payload, so a payload field could override routing.

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

- `id` is **required** on a request and **forbidden** on a send. A missing, non-string, empty or over-64-character id is a hard `invalid-request`. (1.x silently demoted a bad id to fire-and-forget, turning a client bug into a request that never settled.)
- A present `payload` must be an object; a non-object payload is `invalid-request`, not something to coerce.
- Endpoint kind is enforced structurally. A `request()` naming a send endpoint rejects `wrong-endpoint-kind`; a `send()` naming a request endpoint is **dropped** — but never silently: it comes back to your console (see *Errors*).
- Malformed messages are rejected and logged, never fatal. Don't flood the bridge — it shares the game thread — and note one view may have at most 64 requests in flight before the next is rejected `request-capacity`.

### The helper surface

This is all of it:

```js
osfui.available                       // PROPERTY (not a call): false = plain browser
const info = await osfui.ready;       // RuntimeInfo; rejects "no-bridge" standalone

osfui.send("close");                                            // → boolean ("posted")
const reply = await osfui.request("menu.open", { view: id });   // → the reply PAYLOAD
const offEvent = osfui.on("ui.hotkey", (p) => { … });           // → unsubscribe
const offState = osfui.state.on("osfui/settings", render);      // → unsubscribe
const now = osfui.state.get("yourname.mymod/credits");          // latest value, or undefined

osfui.markReady();                                  // manifests with readySignal:true
osfui.papyrus.call("MyModUI", "Equip", formId, 1); // arbitrary GLOBAL function; fire-and-forget
osfui.papyrus.send("equip", formId, 1);             // one-way, to your own mod's script
const price = await osfui.papyrus.request("price", formId);

await osfui.i18n.ready;                             // first catalog has arrived
osfui.i18n.locale;                                  // PROPERTY, e.g. "de"
osfui.i18n.t("views.myhud.heading", "Ship status");
osfui.i18n.localize(root);                          // apply data-i18n* below a root
osfui.theme.applyAccent(el, "#e6904a");             // never touches the wire
```

Two things about `request()`: it resolves the **reply payload**, not an envelope (correlation ids are private), and it rejects with an `Error` carrying a stable machine `.code`, so `catch (err) { if (err.code === …) }` is the intended shape. The client timer defaults to 10 000 ms; `{ timeoutMs: 0 }` disables *only* the client timer — the host still answers `no-response` at its own 30 s deadline, so a request can't hang forever.

`state.on()` replays the current cached value **synchronously** at subscribe time if one has arrived, then fires on every change. Subscribing is a read.

State keys are `"<modId>/<key>"` — the owning mod, then the name the backend published. Platform keys are `osfui/…`, yours are `yourname.mymod/…`. Keys match case-insensitively on both halves, because a Papyrus key arrives through `BSFixedString` interning, which hands back the first casing the process saw.

> **Gone from 1.x**, and gone loudly (`not a function`): `osfui.emit`, `osfui.call`, `osfui.action`, `osfui.viewReady`, `osfui.data.*`, top-level `osfui.t` / `localize` / `locale()` / `i18nReady` / `applyAccent`, and `available()` as a *call*. The one break that fails **silently** is `request()`: it used to resolve the whole envelope and now resolves the payload, so `reply.payload.x` becomes `reply.x`.

Under the helper sit two primitives (all the helper itself uses):

```js
window.osfui.postMessage(jsonString);          // web → native: one JSON message
window.osfui.onMessage = (jsonString) => { };  // native → web: OWNED BY THE HELPER
```

With `shared/osfui.js` loaded, never assign `onMessage` yourself — subscribe with `osfui.on()` / `osfui.state.on()`. (Messages arriving before anything is assigned to `onMessage` are queued by the injected shim and flushed on assignment, which is why loading the helper in `<head>` or before your script is enough.)

### Sends (web → native, one-way)

Anything not listed is dropped and surfaced as `unknown-endpoint`.

| name | payload | effect |
|---|---|---|
| `osfui.hello` | — | greet the bridge. **The helper sends this for you** on every document |
| `close` | — | close the calling surface (closing the last open menu hides the overlay; a coexisting live HUD stays up) |
| `setVisible` | `visible: bool` | open/close the calling surface |
| `view.ready` | — | declare this page has meaningful content ready for its first reveal; only for manifests with `readySignal:true`. Sugar: `osfui.markReady()` |
| `log` | `text: string` | write to `OSF UI.log` (truncated to 512 chars) |
| `osfui.gamepadRaw` | `raw: bool` | *(experimental — exempt from the stability guarantee)* take over gamepad handling: suppress the default nav mapping and consume raw `ui.gamepad` events. Cleared whenever your document greets the bridge, i.e. on every (re)load — re-assert it from ordinary boot code, not a reload handler; there is no reload handler |
| `osfui.handleBack` | `handle: bool` | own the back action. While your menu is ACTIVE, Esc / gamepad B are delivered to your page as a synthetic Escape keydown/keyup instead of closing the top menu — handle it and decide: navigate (`menu.open`), dismiss an inner panel, or `send("close")`. Same per-document lifetime as `osfui.gamepadRaw`. The overlay toggle key always closes natively, so a page that stops responding can't strand the player |
| `papyrus.call` | `script: string`, `function: string`, scalar `args?` | queue an arbitrary GLOBAL Papyrus function. JavaScript integers become Papyrus `int`, fractional numbers become `float`, and strings/booleans retain their types; use `osfui.papyrus.float(3)` when a whole-valued number must be a `float`. Fire-and-forget: use `SetView*` or `SendViewEvent` for observable results. Sugar: `osfui.papyrus.call(script, function, ...args)` |
| `papyrus.send` | `name: string`, `args?: (string\|number\|boolean)[]` | one-way message to the OWNING mod's Papyrus listener, delivered as `OnOSFUIViewAction(name, args)`. The target mod is derived from the calling view's id — the payload can't spoof it. Sugar: `osfui.papyrus.send(name, ...args)`. See [authoring-dynamic-data.md](authoring-dynamic-data.md) |
| `osfui.handoffRetry` | — | *(platform-private)* retry a stalled first-load handoff |

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
| `papyrus.request` | `name`, `args?` | `{ value }` — the sugar unwraps it to `value` | `invalid-request`, `papyrus-unavailable`, `papyrus-timeout`, or the script's own `RejectViewRequest` code |

Two that surprise people:

- **`menu.open` resolves "accepted and queued".** The open lands on the next tick, through the same snapshot/load/pump path as the native `RequestMenu`. That's all a caller can act on; the alternative would be a reply waiting an arbitrary number of frames on a page load.
- **`settings.captureKey` settles immediately; the key arrives later.** It resolves the moment native capture is *armed*; the key the player presses comes back as the `settings.captured` **event**, however many seconds later. A request left pending on a human fights the client timeout and makes "waiting for you" indistinguishable from "the backend died". Requests settle in machine time; human-time outcomes are events. (There is consequently no `timeoutMs: 0` usage left in 2.0.)

> There is intentionally no "call any native function" escape hatch. New endpoints come from native code only: a handler in the OSF UI runtime, or a separate SFSE plugin registering its own through the native bridge API ([native-plugin-api.md](native-plugin-api.md)). Plugin endpoints must be shaped `<author>.<modname>.<name>` (two dots minimum; the leading mod id follows the §0 grammar), so no mod can register a platform name and no registry is needed to keep the namespaces apart.

### State keys (`osfui.state.on(key, fn)`)

Every one of these replays to every fresh document. Nothing here is requested or re-requested after a reload.

| key | value | when it changes |
|---|---|---|
| `osfui/settings` | `{ mods: [{ id, title, schema, values, shadowed?, targetVersion? }], keyboard?, loadErrors? }` | the registry SHAPE changes (a schema registers, hot-reloads or goes away), a whole-mod reset lands, or the OS keyboard layout switches. Individual commits arrive as the `settings.changed` event. `keyboard` is `{ layout, labels: { keyName -> keycap } }` — the player's localized keycaps (display only; fall back to the name when absent). `loadErrors` names settings files that failed to load, so a surface can say so instead of a mod silently vanishing |
| `osfui/keybindings` | `{ available, revision, gameVersion, error?, actions: [{ event, label, category, context, classification, modes, sortIndex, required, bindings }] }` | the live engine ControlMap is first copied or remapped. Complete read-only panel order, including unbound, main/alternate, and chorded keyboard rows. `classification` is `core` \| `special` \| `menu` \| `unknown`; `bindings[].key` is an OSF UI physical name or `null`. `available:false` is fail-closed and carries no actions |
| `osfui/input-context` | `{ available, revision, mode, contexts: [{ id, name }] }` | the exact active engine context stack or derived semantic mode changes. `mode` is `onFoot` \| `ship` \| `vehicle` \| `zeroG` \| `null` |
| `osfui/views` | `{ views: [{ id, title, description, mod, kind, interactive, hub, targetVersion, open, focused, loadState, autoStart, autoStartMutable, pinned }] }` | any open/close/focus/load-state change. `loadState` is `"unloaded"` (discovered on disk, never loaded — still listed so a launcher can offer it) \| `"loading"` \| `"loaded"` \| `"failed"`. Respect `hub:false` — don't list those |
| `osfui/diagnostics` | `{ system, issues: [{ id, code, severity, status, source, subject, context, occurrences, firstAt, lastAt, resolvedAt? }] }` | the session health registry changes. Each issue carries a stable machine `code` — map it to your own copy, the payload never contains player-facing prose. Powers the Mods surface's System Health destination; a normal content view rarely needs it |
| `osfui/i18n` | `{ mod, locale, strings }` | language change or a devMode catalog reload. **Per view**: the value is your own mod's catalog. The helper consumes this for you — use `osfui.i18n.t()` |
| `osfui/handoff` | `{ target, mod, title, accent, phase, retry }` | *(platform-private)* the first-load handoff surface's own state |
| `<yourModId>/<key>` | whatever your backend published | your Papyrus `OSFUI.SetView*` or your plugin's `SetViewState`. See [authoring-dynamic-data.md](authoring-dynamic-data.md) |

### Events (`osfui.on(name, fn)`)

| name | payload | when |
|---|---|---|
| `settings.changed` | `{ mod, key, value, conflicts? }` | one committed value changed — whoever wrote it (settings menu, preset, reset, Papyrus, native). `value` is post-validation and therefore authoritative. On a `key`-typed setting, `conflicts` is the recomputed collision list (always present, `[]` = none) so badges update without re-reading the registry. You receive changes for **all** mods — filter on `mod` |
| `settings.persisted` | `{ mod }` | that mod's values file hit disk (writes coalesce ~500 ms). Drives a "Saved" indicator |
| `settings.captured` | `{ mod, key, name, cancelled, label?, reason?, conflicts? }` | the outcome of a `settings.captureKey`: the captured key `name` (an OSF UI key name — a physical position), or `cancelled:true` (keep the old binding; `reason` says why: `"escape"` \| `"reserved"` \| `"unnameable"`). `label` is the player's keycap for `name` (display only — show it, commit `name`). `conflicts` lists collisions this bind *would* create; warn live, never block. The view then sends a normal `settings.set` with `name` |
| `ui.hotkey` | `{ mod, key }` | the physical key bound to that `key`-typed setting was pressed in-game. Delivered to every bridged view — filter on `mod`. Suppressed while the overlay captures input or a rebind is armed |
| `ui.visibility` | `{ visible, reason? }` | this view was shown/hidden as the overlay's focused menu (edge-triggered). Fires on overlay open/close AND on a `menu.open` view switch while the overlay stays up: the outgoing view gets `visible:false`, the incoming one `visible:true`. `reason` is `"overlay"` or `"focus"`. Treat any `visible:false` as a real hide |
| `ui.gamepad` | `{ kind:"button", button:{id, down} }` \| `{ kind:"stick", axes:{lx, ly, rx, ry} }` | *(experimental)* raw gamepad events to the ACTIVE view while the overlay captures input. The default nav mapping also applies unless you asserted `osfui.gamepadRaw` |
| `<modId>.<name>` | `{ args: string[] }` | a one-shot happening announced by the owning mod's Papyrus (`OSFUI.SendViewEvent`) or its native plugin (`SendToWeb`). Never cached, never replayed |
| `osfui.debug.error` | `{ code, message, detail? }` | **devMode only**, and the helper intercepts it: it prints to your console and does *not* deliver it to `on()` handlers |

Ignore event names you don't know (and never `eval` them), including ones a future runtime may add.

### Errors and how you find out about them

Every `request()` rejection carries a stable `code`. The layers stay distinguishable because they point at different culprits:

| code | meaning |
|---|---|
| `no-bridge` | local and immediate — a plain browser, or `nativeBridge:false` |
| `timeout` | the CLIENT timer gave up (default 10 s) |
| `no-response` | the BACKEND missed the host-side 30 s deadline |
| `wrong-endpoint-kind` | `request()` on a send endpoint (or `send()` on a request one) |
| `unknown-endpoint` | no such endpoint |
| `invalid-request` | malformed envelope: bad kind, empty name, bad/missing id, non-object payload |
| `request-capacity` | this view already has 64 requests in flight |
| anything else | the handler's own code (`unknown-view`, `capture-busy`, a Papyrus `RejectViewRequest` code, …) |

**Debugging is F12 Chromium DevTools** (devMode only). OSF UI builds no inspector of its own; instead everything an author can get wrong *reaches* the page console, where DevTools shows it with full object inspection — and, because devMode forwards console output over the host pipe, in `OSF UI.log` at the same time.

- The helper prints every request rejection, every `no-bridge` and every client timeout with an `[osfui]` prefix, so an unhandled promise rejection is preceded by a named, inspectable error.
- Mistakes the page would otherwise never hear about — a `send` dropped for naming a request endpoint, a send to an unknown endpoint, a backend that missed its deadline — come back as the dev-only `osfui.debug.error` event, printed the same way.
- Release builds have no debug channel, so *repetition* is the signal: a view that keeps getting the protocol wrong raises a `view.protocol-misuse` health card on the Mods surface. A one-off stays out of the player's face.
- For "what is actually crossing the bridge", set `localStorage["osfui:trace"] = "1"` and reload. The helper then logs every envelope both directions via `console.debug`, including each request's settlement latency. This also answers "why is my HUD blank": every replayed `state` envelope is visible at document boot, so either the key arrives (view bug) or it doesn't (backend bug).

### Minimal example

```html
<script src="../../shared/osfui.js"></script>
<script>
"use strict";

// State: no request, no re-request after a reload. The handler runs immediately
// with the current value once it has arrived, and again on every change —
// including on the fresh document after an F5.
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

[`frontend/src/views/osfui/settings/`](../frontend/src/views/osfui/settings/) is a complete, commented example — the *source* of the built-in Mods surface. The copy under `build/frontend/views/osfui/settings/` is disposable build output; inspect it for the exact shipped bytes, never edit it.

---

## 4. Settings schemas (the MCM-style path)

> The full author guide — quickstart, every widget, presets, hotkeys, localization, update strategy, testing — is [authoring-settings.md](authoring-settings.md). This section is the protocol-level summary.

Drop a JSON schema at `SFSE/Plugins/OSFUI/settings/<author>.<modname>.json`. Every schema in that folder loads as a separate "mod" and renders as its own card in the built-in `settings` view, with no per-mod native or web code. Values persist per mod to `SFSE/Plugins/OSFUI/settings/values/<id>.json` (VFS-captured, so per-profile under MO2), survive relaunch, and the runtime can react to changes natively.

### Schema format

```jsonc
{
  "id": "yourname.mymod",        // optional, defaults to the filename stem; "<author>.<modname>" grammar (§0)
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

A mod that disables Starfield gameplay controls during a modal state can declare a named context once and reference it from each scene-only key:

```json
{
  "inputContexts": [
    { "id": "scene", "label": "During on-foot scenes", "blocksGameplay": true,
      "gameplayModes": ["onFoot"] }
  ],
  "groups": [{ "settings": [
    { "key": "progressScene", "type": "key", "default": "Space", "inputContext": "scene" }
  ] }]
}
```

`gameplayModes` controls dispatch through stable semantic modes: `onFoot`, `ship`, `vehicle`, and `zeroG`. Scoped keys fire only when the live engine stack proves a listed mode; they fail closed if that provider or mode is unavailable. Missing, malformed, empty, or unknown lists preserve legacy unscoped non-menu dispatch and warn in the log. `blocksGameplay` separately marks vanilla collisions as expected shares. Mod keys with overlapping modes still conflict, while proven-disjoint mode sets may share. Context ids are local to the mod, must match `[A-Za-z0-9][A-Za-z0-9._-]{0,63}`, and can't be `gameplay`.

### Type rules (enforced natively in `SettingsStore`)

| type | control | validation on write |
|---|---|---|
| `bool` | toggle switch | must be a boolean |
| `int` | slider | number, clamped to `[min,max]`, rounded |
| `float` | slider | number, clamped to `[min,max]` |
| `enum` | dropdown | must be one of `options` |
| `flags` | checkbox group | array of `options` strings (multi-select). Unknown options and duplicates are filtered, and the stored array is canonicalized to declared-option order |
| `string` | text field | truncated to 256 chars |
| `key` | press-to-bind button | key-name string (≤16 chars) denoting a PHYSICAL key position (US reference keyboard, like the engine controlmap — layout-independent; display the localized keycap from `keyboard.labels`), non-empty unless the setting sets `"allowUnbound": true` — then `""` is the deliberate unbound state (no hotkey dispatch, no conflict badges, and the UI adds an unbind ×). Framework-managed: capture is armed via `settings.captureKey` and grabbed in the native input layer, so pressing the current toggle key rebinds instead of closing the overlay. Every `key`-typed setting of every mod is rebindable and dispatches via the HotkeyService (`ui.hotkey` / `SubscribeHotkey`); optional immutable `onPress: {script,function}` metadata additionally queues that GLOBAL Papyrus callback after the normal channels |

This is the frozen base type set. There's no `color` type; use `type:"string"` + `widget:"color"`. Evolution is a base type plus a `widget` hint and attributes; a genuinely new base type ships in a new OSF UI version your schema names via `targetVersion`.

**Forward compatibility.** A host predating one of your setting types renders that row read-only ("needs a newer OSF UI"), serves the schema `default` to consumers, and preserves the user's saved value untouched — it round-trips through every rewrite, and a newer host picks it back up. If your schema uses anything newer than the OSF UI you tested against, declare that version so older hosts tell the user to update:

```jsonc
{ "id": "yourname.mymod", "targetVersion": "2.0.0" }
```

Same advisory field as the view manifest's (§2): the schema still loads best-effort, but the Mods surface shows the "needs update" badge with your mod named, and the detail pane notes some settings may be unavailable until OSF UI is updated.

Values files carry two reserved `$`-prefixed meta keys owned by the host: `$schemaVersion` (your schema's `version`, stamped on write) and `$formatVersion` (the sparse encoding's own version). Never name a setting with a leading `$`; unknown `$`-keys from newer hosts round-trip like any preserved value.

`bool` renders as a toggle switch, `int`/`float` as sliders with a value badge; see `views/shared/osfui.css` for the shared control styles.

Unknown keys, wrong types and out-of-range values are rejected or clamped server-side, and `settings.set` **rejects** with the code (`unknown-setting`, `read-only`, `invalid-value`). Untrusted JS can't write arbitrary keys, out-of-range values, or to any path but its own settings file: a view may only write **its own mod's** settings (`forbidden` otherwise) — only the built-in Mods surface and keybinds board may name a foreign mod, because editing other mods' settings is their entire purpose.

### Reacting to changes from your view

The registry is the `osfui/settings` state key; committed values are the `settings.changed` event. That pairing is the whole subscription model — no read call, nothing to redo after a reload:

```js
osfui.state.on("osfui/settings", (data) => {
  const mine = data.mods.find((m) => m.id === "yourname.mymod");
  if (mine) applyAll(mine.values);            // replayed on every document
});

osfui.on("settings.changed", (p) => {
  if (p.mod === "yourname.mymod") applySetting(p.key, p.value);
});
```

The event's `value` is post-validation and therefore authoritative; on `key`-typed settings its `conflicts` list is recomputed, so badges update without re-reading the registry. You receive changes for all mods — filter on `p.mod`. A mod's HUD therefore reacts live to its own settings with zero polling and zero native code.

### Hotkeys

Every `type:"key"` setting is a live hotkey: when the user presses the bound key in-game, the runtime raises `ui.hotkey { mod, key }`. So this makes your HUD toggleable:

```js
let visible = false;
osfui.on("ui.hotkey", async (p) => {
  if (p.mod !== "yourname.mymod" || p.key !== "toggleHud") return;
  visible = !visible;
  await osfui.request(visible ? "menu.open" : "menu.close",
                      { view: "yourname.mymod/hud" });
});
```

Presses never fire while the overlay captures input (typing in an overlay text field, say) or while a rebind capture is armed; key repeats never fire; the user can rebind freely and the runtime re-resolves the binding on every change. Duplicate bindings across mods all fire — the settings view badges them (the `conflicts` data above) but never blocks them.

### Reacting natively (C ABI)

Separate SFSE plugins subscribe over the native bridge with no core edit: `SubscribeSettings` plus typed getters for values, and `SubscribeHotkey` for key presses ([native-plugin-api.md](native-plugin-api.md) §8a/§8b). In-tree framework knobs still react through `Runtime::OnSettingChanged` (e.g. `osfui.toggleKey` live-rebinds the overlay's open/close key).

---

## 5. Testing locally

For a new view, use the one-command npm workflow in [view-toolchain.md](view-toolchain.md):

```bat
npm create osfui@latest my-view
cd my-view
npm run dev
```

It provides the browser harness below plus source presets, Vite HMR, generated manifests, in-game sync, temporary author mode, validation and packaging.

### Browser harness

`npm run dev` serves the view at the same `/<modId>/<viewName>/<entry>` URL shape used in game, generates and validates `manifest.json` from `osfui.config.ts|js`, supplies OSF UI's public `shared/osfui.js` and `shared/osfui.css`, and installs a mock backend speaking the real 2.0 protocol — page-initiated handshake included, so a harness reload exercises the same boot path the game does.

The toolbar provides:

- manifest and custom resolutions, scaled down without changing page layout;
- visible/hidden lifecycle edges, locale changes, page reload, a transparency checkerboard;
- a bridge traffic inspector — one scannable row per envelope (`send · close`, `request · settings.set`, `state · osfui/settings`, `hotkey · F9`, `error · unknown-endpoint`) that expands to the raw envelope on click, pairs each request with its reply and round-trip time, folds repeats into a `×N` counter, and can be filtered or paused.

Saving view source updates the page through Vite HMR. Development responses use `Cache-Control: no-store`. A document CSP blocks remote resources, and the bootstrap removes WebRTC, WebTransport and worker constructors to catch unsupported dependencies. WebSocket stays available only for Vite's loopback HMR connection; `osfui check` rejects authored uses of unsupported transports.

For repeatable backend data, describe what your backend would do in the project's mock (`osfui.mock.ts` with `defineMock`, or a plain `osfui.mock.json`, beside `osfui.config.ts`); the browser reloads when it changes:

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

Each `state` entry publishes under **your own mod id** — `credits` arrives as `yourname.mymod/credits` — and is replayed right after `ready`, exactly as the runtime replays it. A `requests` entry answers that request endpoint (values may be plain JSON or, in a `.ts`/`.js` mock, a function of the payload); prefix a key with `papyrus.` to answer `osfui.papyrus.request("…")`. Unconfigured endpoints aren't faked: a request rejects `mock-unhandled`, and a send to an unknown name surfaces `unknown-endpoint`. A mock answering a name your view *sent* one-way is flagged as a mock authoring mistake, because a send has nothing to settle. Named `scenarios` shallow-overlay these fields (`?scenario=<name>`); see [view-toolchain.md](view-toolchain.md). The mock lives at the project root, so it can never ship with the views.

For a minimal standalone fallback, a view can detect a missing bridge:

```js
if (!osfui.available) {
  // running in a plain browser — stub or no-op the native calls
}
```

(`shared/osfui.js` installs itself even without a bridge, so `osfui.available`, `osfui.on()` and `osfui.state.on()` are always safe to touch; `osfui.ready` and `osfui.request()` reject with `"no-bridge"`, and the helper logs one `[osfui]` notice — a plain-browser preview is not an authoring mistake.)

Serve that fallback over `http://` rather than `file://`, so local testing uses a normal origin like the in-game `https://osfui.local` mapping.

### Built-in OSF UI views

The built-in views (`osfui/settings`, `osfui/keybinds`) aren't openable this way — their shipped `index.html` is generated output. They're developed through the **same `osfui dev` harness this guide describes**: the frontend directory is itself an `@osfui/cli` project whose mock module (`frontend/osfui.mock.ts`) speaks the real protocol.

```bat
npm --prefix frontend run dev
```

See [`frontend/README.md`](../frontend/README.md) for deep-link URLs and locale/fixture/stage switches.

In-game, watch `Documents\My Games\Starfield\SFSE\Logs\OSF UI.log`:

- `MessageBridge: [web] ...` — your `log` sends.
- `MessageBridge: view '<id>' greeted — ready, state replay, events open` — the handshake completed for that document.
- `MessageBridge: [content] dropped send to unknown endpoint '...'` / `rejected request to unknown endpoint '...'` — you named something that doesn't exist (logged once per name; your page's console gets it every time).
- `devMode: true` in `config.json` adds verbose renderer and bridge logging.

With `devMode: true` the in-game loop is fast too:

- **Settings schemas hot-reload**: edits to `settings/*.json` are picked up in about a second. Values are preserved (a renamed key carries over via its `aliases`), an open settings view repaints, and deleting the file drops the mod. A runtime-registered (DLL) schema is never touched by files.
- **Loose view auto-reload**: save HTML, JS, CSS or a local asset in a loaded view and OSF UI reloads it after the file settles (normally within half a second). The replacement document greets the bridge itself and is replayed all of its state, so a hot-reload isn't a special case. Polling and MO2 mirror synchronization happen in the background; removed or renamed files disappear from the mirror. It deliberately ignores `manifest.json` changes and new view folders, which need a game restart. Built-in views still require `npm --prefix frontend run build`.
- **DevTools (`F12`)**: opens Edge DevTools for the top open menu. Available only in `devMode`; outside authoring sessions the browser capability is disabled.

---

## 6. Checklist for shipping a view

- [ ] `views/<modId>/<viewName>/manifest.json` — folder names pass the id grammar (§0), `permissions.nativeBridge` set as needed. The folder name IS the view id — the manifest declares none.
- [ ] Responsive CSS (no hardcoded 1280×720 assumptions; the view is resized to the screen).
- [ ] All assets local and relative (no `..`, no absolute paths, no network) — plus the sanctioned `../../shared/osfui.css` / `osfui.js`.
- [ ] Load `shared/osfui.js` before your script. Declare the `targetVersion` you authored against; use `"2.0.0"` or later after migrating to the strict four-verb surface.
- [ ] **No lifecycle code.** Everything the page renders comes from `osfui.state.on()`; nothing is re-requested on load, and nothing depends on `osfui.ready` having fired first.
- [ ] Verbs chosen by semantics: `request()` (and its rejection `code`) where the outcome matters, `send()` where it can't fail, `on()` only for happenings.
- [ ] (If configurable) a `settings/<modId>.json` schema with sane `default`/`min`/`max`.
- [ ] Verified standalone in a browser, then in-game — including a mid-session F5 (devMode) to prove the page repaints from state alone.

---

## 7. Schemas & type definitions

- **JSON Schemas** ([`docs/schema/`](schema/)) validate your files in any editor that understands JSON Schema:
  - [`manifest.schema.json`](schema/manifest.schema.json) — `views/<modId>/<viewName>/manifest.json`
  - [`settings-schema.schema.json`](schema/settings-schema.schema.json) — `settings/<id>.json`

  Point your editor at them (VS Code `json.schemas`, or a top-level `"$schema"` key) for autocomplete and validation.

- **TypeScript definitions** ([`sdk/osfui.d.ts`](../sdk/osfui.d.ts)) type `window.osfui`, the send/request endpoint whitelists, the platform state keys and events, and the settings-schema shapes. Reference it from your view's TS project and the bridge is typed globally — no package to install.

### Versioning

Declare what you authored against; degrade gracefully at runtime. One advisory field, `targetVersion`, appears in both author-facing files (view manifest §2, settings schema §4). It never blocks loading: it tells the Mods surface when the running OSF UI is older than your target, and selects the compatibility helper when a view target predates 2.0. The running host's version arrives as `ready`'s `version` if your code must branch on it:

```js
const info = await osfui.ready;
console.log(`running OSF UI ${info.version} (bridge ${info.bridgeVersion})`);
```

The protocol version is **2.0**, emitted as `bridgeVersion` — informational (logs, bug reports), distinct from the plugin `version`. Additive changes bump the minor; anything that would break a shipped view bumps the major. The constant lives in `src/core/Version.h` (`kBridgeProtocolVersion`); the schemas, `.d.ts` and the shared helper are kept in lockstep with it (CI greps the docs against the constant).
