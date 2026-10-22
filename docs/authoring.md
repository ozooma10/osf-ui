# Making content for OSF UI

OSF UI lets a Starfield mod show an HTML page in the game, as a menu or as a HUD
layer, and talk to it from Papyrus or from an SFSE plugin. This guide walks through
building one, end to end. The exact signatures live in the reference files:

| Reference | Covers |
| --- | --- |
| [osfui.d.ts](../sdk/osfui.d.ts) | The page-side `osfui` object |
| [OSFUI.psc](../data/Scripts/Source/OSFUI.psc) | Papyrus functions, with usage notes in the comments |
| [OSFUI.h](../sdk/OSFUI.h) | The C++ API for SFSE plugins |
| [manifest.schema.json](schema/manifest.schema.json) | `manifest.json` fields (point your editor's `$schema` at it) |

OSF UI does not handle settings, hotkeys, or the mod menu. Those belong to
[OSF Settings](https://github.com/ozooma10/osf-settings-slim), which players
install alongside OSF UI. See [Settings and hotkeys](#settings-and-hotkeys).

## Pick a shape

| You want | You ship |
| --- | --- |
| A page that only shows static content, listed in the OSF Settings launcher | A view folder |
| A page that calls a Papyrus `Global` function without a reply | A view folder plus a compiled script; no quest or registration needed. See [Direct Global calls](#direct-global-calls). |
| A page that exchanges requests, events, or state with Papyrus | A view folder plus a Papyrus script (with a quest to register it) |
| Native speed, engine access, or relative mouse input | A view folder plus an SFSE plugin |

All use the same page API. Registered endpoints can be implemented in Papyrus or C++.

## 1. Your first view

A view is a folder containing a `manifest.json`. It lives at
`Data/SFSE/Plugins/OSF/UI/views/<mod-id>/<view-name>/`:

```text
Data/SFSE/Plugins/OSF/UI/views/
  mymod/
    panel/
      manifest.json
      index.html
      main.js
      style.css
```

The view ID is `mymod/panel`. It comes from the folder names only: the manifest's
`mod` field is just a hint and never changes the ID.

`manifest.json`:

```json
{
  "$schema": "https://github.com/ozooma10/osf-ui/blob/main/docs/schema/manifest.schema.json",
  "manifestVersion": 1,
  "title": "My Panel",
  "kind": "menu",
  "launcher": true,
  "width": 900,
  "height": 560
}
```

`index.html`:

```html
<!doctype html>
<html lang="en">
<head>
  <meta charset="utf-8">
  <link rel="stylesheet" href="style.css">
  <script src="/shared/osfui.js"></script>
  <script src="main.js" defer></script>
</head>
<body>
  <main>
    <h1>Hello from My Panel</h1>
    <button id="close">Close</button>
  </main>
</body>
</html>
```

`main.js`:

```js
document.getElementById("close").addEventListener("click", () => osfui.send("close"));
```

Install the folder as an MO2 mod and start the game. With `"launcher": true`, the menu
appears in OSF Settings' launcher under its title. Escape or gamepad B closes it.

OSF UI scans for views once, at startup. **Adding a view or editing any
`manifest.json` needs a game restart.** Changes to other files can reload live (see
[Developing and debugging](#developing-and-debugging)).

### ID rules

- **Mod ID** (`mymod`): 1–64 characters from `A–Z a–z 0–9 - . _ ~`. Lookups ignore
  case. `osfui` is reserved, and Windows device names (`con`, `nul`, `com1`, …) are
  rejected. **Use lowercase.** Papyrus lowercases mod IDs, and the launcher requires
  `[a-z0-9._-]`.
- **View name** (`panel`): 1–64 characters from lowercase `a–z`, `0–9`, and `-`.
  Uppercase is rejected.
- A view whose folder names or manifest are invalid is skipped. The reason is logged
  as `ViewManifest: [content] …` in `OSF UI.log`. Other views still load.

### Manifest fields

| Field | Default | Meaning |
| --- | --- | --- |
| `manifestVersion` | required | Must be `1`. |
| `title`, `description` | | Shown in the launcher. |
| `entry` | `index.html` | Relative path inside the view folder. Absolute paths and `..` are rejected. |
| `width`, `height` | `1600`, `900` | Page size in pixels, 1–16384. |
| `transparent` | `true` | Transparent page background, composited over the game. |
| `kind` | `menu` | `menu` takes focus. `hud` is a passive overlay layer. |
| `capturesInput` | `true` | Menu only. The menu receives mouse, keyboard, and gamepad input, and the game doesn't. |
| `pausesGame` | `true` | Menu only. Pauses gameplay while the menu is open. |
| `launcher` | off | Menu only. `true` lists the menu in OSF Settings. `{ "modId": "...", "modTitle": "..." }` overrides the defaults (the folder name and `title`). |
| `openOnStart` | `false` | HUD only. Shows the HUD automatically once the game is loaded. |
| `order` | `0` | HUD only. Paint order among HUDs, 0–999. Menus always draw on top. |
| `debugOnly` | `false` | HUD autostart and the launcher entry apply only when OSF UI developer mode is on. |

Setting `capturesInput` or `pausesGame` on a HUD logs a warning, and both are forced
off.

## 2. Talking to the page

Load `/shared/osfui.js` before your own scripts. It defines `window.osfui` and
connects to the game. The page side has four verbs:

| Page | Direction | Backend side |
| --- | --- | --- |
| `osfui.send(name, …)` | page → backend, one-way | Papyrus `RegisterSend`, C++ `RegisterSend` |
| `await osfui.request(name, …)` | page → backend → page | Papyrus `RegisterRequest` + `Reply`/`Reject`, C++ `RegisterRequest` |
| `osfui.on(name, fn)` | backend → page, transient | Papyrus `EmitEvent`, C++ `EmitEvent` |
| `osfui.state.on(key, fn)` / `osfui.state.get(key)` | backend → page, retained | Papyrus `SetState`, C++ `SetState` |

```js
osfui.send("equip", 2);                                     // payload { args: [2] }
osfui.send("equip", { slot: 2 });                           // payload { slot: 2 }
const count = await osfui.request("getCount");
const sum = await osfui.request("sum", 2, 3, { timeoutMs: 2000 });
const off = osfui.on("toast", (p) => show(p.args[0]));      // returns an unsubscribe function
osfui.state.on("credits", (v) => render(v));                // replays the current value immediately
```

**State or event?** Use state for anything the page needs in order to draw itself:
counts, lists, flags. Each new or reloaded page gets the latest value replayed, so
it's never missing. Use events only for momentary things like "play a sound" or
"flash this row". An event that arrives while no page is open is lost.

**Names.** A page's call to `"equip"` looks up `"<your-mod>.equip"` first, then
`"equip"` as written. Register endpoints under your mod and call them with the short
name. A page can reach another mod's endpoint by its full name, for example
`"othermod.getCount"`. The names `close`, `setVisible`, `ping`, `menu.open`,
`menu.close`, `setViewHidden`, `papyrus.call`, and anything starting with `osfui`
are reserved.

**Errors.** A failed `request` rejects with an `Error` that has a `code`. The codes
are your own `Reject` codes, or `timeout` (10 s by default; `{ timeoutMs: 0 }`
disables it), `game-load` (a save loaded while the request was in flight),
`no-bridge`, `wrong-endpoint-kind`, `invalid-payload`, or `papyrus-unavailable`. Failures are also printed to the page console.

**Limits.** Each page message can be at most 64 KiB, and a page can send at most 128
messages per second. Native events and state can be up to 1 MiB each.

## 3. A Papyrus backend

For requests, events, and retained state, use the registered backend below. For a
one-way call to a `Global` function, see [Direct Global calls](#direct-global-calls).

Papyrus registrations last for one session only. **They are cleared on every game
load and on return to the main menu.** Register from a quest that starts with the
game, and register again on every load:

```papyrus
ScriptName MyMod:Bridge extends Quest

Event OnInit()
    RegisterForRemoteEvent(Game.GetPlayer(), "OnPlayerLoadGame")
    Setup()
EndEvent

Event Actor.OnPlayerLoadGame(Actor akSender)
    Setup()
EndEvent

Function Setup()
    OSFUI.RegisterSend(self, "mymod", "equip")
    OSFUI.RegisterRequest(self, "mymod", "getCount")
    OSFUI.SetState("mymod", "credits", 1200)
EndFunction

; page: osfui.send("equip", 2)
Function OnOSFUISend(string asName, Var[] akArgs, string asSourceViewId)
    If asName == "equip" && akArgs.Length > 0
        Debug.Trace("equip slot " + (akArgs[0] as int))
    EndIf
EndFunction

; page: await osfui.request("getCount")
Function OnOSFUIRequest(string asName, Var[] akArgs, string asSourceViewId, string asReplyToken)
    If asName == "getCount"
        OSFUI.Reply(asReplyToken, 3)
    Else
        OSFUI.Reject(asReplyToken, "unknown-request", asName)
    EndIf
EndFunction
```

The registration functions return `1` on success and a negative code on failure.
`OSFUI.psc` lists the codes.

Papyrus details:

- **Positional arguments only.** Papyrus endpoints receive `payload.args` as
  `Var[]`. Call them as `osfui.send("equip", 2, 3)`, not with an object. Arguments
  can be null, bool, int, float, string, or `{ formId: n }`, which arrives as a
  `Form`.
- **Settle every request exactly once.** Answer with `Reply` (or `ReplyInts`,
  `ReplyStrings`, `ReplyForms`, and so on) or with `Reject`. A request you never
  answer leaves the page waiting until it times out.
- **Forms reach the page as objects**: `{ formId, name?, editorId? }`.
- **Event names are lowercased.** `OSFUI.EmitEvent("mymod", "ItemAdded", args)`
  arrives as `itemadded`, so listen with `osfui.on("itemadded", …)`. The payload is
  `{ args: [...] }`, and the event goes to every open view of the mod.
- **State is session-scoped too.** Publish it again in `Setup()` after each load.
- `RegisterSendStatic` / `RegisterRequestStatic` take a script name instead of an
  object and dispatch to `Global` functions. You still have to call them on every load.
- `OSFUI.Open("mymod/panel")` and `OSFUI.Close("mymod/panel")` open and close any
  view from script.
- The `SetView*`, `RegisterForViewActions*`, `ListenFor*`, `ReplyView*`,
  `RegisterForHotkey*`, and `Unregister` functions at the bottom of `OSFUI.psc` are
  frozen 1.x compatibility. Don't use them in new mods.

### Direct Global calls

`papyrus.call` lets a page invoke an installed Papyrus `Global` function by script
and function name, without registering an endpoint or creating a quest. Ship the
compiled `.pex` for your script:

```papyrus
ScriptName MyMod:Actions Hidden

Function ShowMessage(string asMessage) Global
    Debug.Notification(asMessage)
EndFunction
```

```js
osfui.send("papyrus.call", {
  script: "MyMod:Actions",
  function: "ShowMessage",
  args: ["Hello from My Panel"]
});
```

This is a **one-way send**. It does not return the function's result or confirm
completion. `send` returning `true` only means the page posted the message to the
bridge. `request("papyrus.call", …)` rejects with `wrong-endpoint-kind`; use a
registered request and `Reply` when you need an answer.

- `script` accepts colon-separated namespaces; `function` is a bare Papyrus
  identifier. Each name is limited to 128 characters.
- `args` is optional (defaults to `[]`), with at most 32 arguments. Arguments can
  be strings, booleans, signed 32-bit integers, or finite 32-bit floats. Unlike
  registered Papyrus endpoints, this call does not accept null, Forms, arrays, or
  arbitrary objects as arguments.
- Whole JS numbers are sent as Papyrus integers. To force a float, including a
  whole value such as `1.0`, use `{ "$papyrus": "float", "value": 1 }` as the
  argument. The shared helper has no `osfui.papyrus.float` method.
- **There is no function allowlist or mod ownership restriction.** Any page can
  target any installed script's `Global` functions that accept these argument
  types. Only the `OSFUI` script itself is blocked, case-insensitively. The local
  file sandbox does not restrict this access to game functions.
- Invalid names or arguments report `invalid-request`; targeting `OSFUI` reports
  `forbidden`; failure to queue the call reports `papyrus-unavailable`. These are
  protocol faults, shown in the page console when developer mode is enabled, not
  Promise rejections or completion replies.

## 4. An SFSE plugin backend

Copy [OSFUI.h](../sdk/OSFUI.h) into your plugin. Acquire the API at SFSE
`kPostPostLoad`, then register endpoints and publish state straight away. You don't
need to wait for `IsReady()`.

```cpp
#include "OSFUI.h"

OSFUI::API::Client ui;

void OnEquip(const char* name, const char* payloadJson, const char* sourceViewId, void*) noexcept
{
    // payloadJson is the page's payload object, e.g. {"args":[2]}
}

void OnGetCount(const OSFUI::API::Request& req, void*) noexcept
{
    req.Reply("3");             // must be valid JSON
    // or: copy `req` and call Reply/Reject later, from any thread. Only the first answer counts.
}

void OnSfseMessage(SFSE::MessagingInterface::Message* msg)
{
    if (msg->type != SFSE::MessagingInterface::kPostPostLoad || !ui.Init()) return;
    ui.RegisterSend("mymod.equip", &OnEquip, nullptr);
    ui.RegisterRequest("mymod.getCount", &OnGetCount, nullptr);
    ui.SetState("mymod", "credits", "1200");
}

// Later:
ui.RequestMenu("mymod/panel", true);                     // open (false closes)
ui.EmitEvent("mymod/panel", "toast", R"({"text":"Saved"})");
```

- Native endpoint names match exactly and are case-sensitive. Register them as
  `<mod>.<name>` so pages can call them by the short name.
- Native `EmitEvent` targets one view (`"mod/view"`), and the event name is used
  as given. Native state, unlike Papyrus state, is not cleared on game load.
- All string arguments are borrowed only for the duration of the callback. Copy
  anything you keep. Keep callbacks short, and don't assume they run on a thread
  where engine calls are safe. Queue engine work instead.
- `RegisterView("mymod/panel")` loads a view folder your plugin ships, if it
  wasn't discovered at startup.
- `RegisterViewLifecycle` reports when a menu is shown or hidden.
  `RegisterRelativePointer` gives you raw mouse deltas while the page holds
  `osfui.send("osfui.relativePointer", { active: true })` (camera-style dragging).
- Request version `kBaseVersion` (API 2.0). Check `Client::Has(...)` before using
  anything added in a later minor version.

The [settings-view example](../examples/settings-view/README.md) is a complete
buildable plugin: it opens a view from an OSF Settings hotkey and forwards a setting
to the page.

## 5. Built-in page controls

These endpoints are always available to a page:

| Call | Effect |
| --- | --- |
| `osfui.send("close")` | Close this view. |
| `await osfui.request("menu.open", { view: "mymod/other" })` | Open another view (menu or HUD). Rejects with `unknown-view` if it doesn't exist. |
| `await osfui.request("menu.close", { view: "mymod/hud" })` | Close a view. If you omit `view`, it closes this one. |
| `await osfui.request("ping")` | Round-trip check. |
| `osfui.send("papyrus.call", { script: "MyMod:Actions", function: "ShowMessage", args: ["Hello"] })` | Invoke a Papyrus `Global` function without a reply. See [Direct Global calls](#direct-global-calls). |
| `osfui.send("osfui.handleBack", { handle: true })` | Escape/B is delivered to the page as an Escape keypress instead of closing the menu. |
| `osfui.send("osfui.handleBack", { handle: true, view: "mymod/main" })` | Escape/B opens that menu instead (for a "back to parent" flow). |
| `osfui.send("osfui.gamepadMode", { mode: "buttons" })` | See [Input](#input). |

`handleBack` and `gamepadMode` reset each time the page loads, so send them on
startup.

Older pages may also use these duplicate controls. Prefer the calls above in new
code:

| Existing call | Preferred control |
| --- | --- |
| `osfui.send("setVisible", { visible: false })` | `osfui.send("close")`; for `visible: true`, use `osfui.request("menu.open")` for this view. |
| `osfui.request("setViewHidden", { view: "mymod/hud", hidden: true })` | `menu.close` with the same `view`; for `hidden: false`, use `menu.open`. Omitting `view` targets this view. |
| `osfui.send("osfui.gamepadRaw", { raw: true })` | `osfui.gamepadMode` with `mode: "raw"`; for `raw: false`, use `mode: "default"`. |

The runtime sends these events to the open menu (HUDs don't receive them):

- `ui.visibility`: `{ visible, reason }`, sent when the menu is shown or hidden.
- `ui.gamepad`: raw button and stick events. See below.

## Input

A menu with `capturesInput` gets the mouse, keyboard, and gamepad. The gamepad maps
onto browser input:

| Mode | D-pad / left stick | A | B | Right stick |
| --- | --- | --- | --- | --- |
| `default` | Arrow keys | Enter | Back | Mouse wheel |
| `buttons` | Arrow keys (D-pad only) | Enter | Back | — |
| `raw` | Nothing is injected. Handle `ui.gamepad` events yourself. B does not close the menu. | | | |

To get controller navigation for free, load `/shared/gamepadnav.js`. It moves focus
spatially between buttons, inputs, links, and `[tabindex]` elements when arrow keys
are pressed, and Enter clicks the focused element. Inside a `[data-nav-modal]`
element, focus stays within it.

**Back** (Escape, or gamepad B): if a `<select>` dropdown is open, Back closes the
dropdown. Otherwise it closes the menu, unless the page has claimed Back with
`osfui.handleBack`.

## HUDs

A `"kind": "hud"` view never takes input and never pauses the game. Any number of
HUDs can be visible beneath one open menu. Show a HUD with `"openOnStart": true`, or
open and close it with `OSFUI.Open`/`Close`, `menu.open`/`menu.close`, or
`RequestMenu`. HUDs hide during loading screens and on the main menu, then come back
on their own.

## Shared assets

`/shared/` is served to every view:

- `/shared/osfui.js`: the bridge helper. Required.
- `/shared/osfui.css`: OSF UI's look. It defines `--osf-*` color tokens and
  `.osf-btn`, `.osf-card`, `.osf-input`, `.osf-switch`, and similar classes.
  Optional.
- `/shared/gamepadnav.js`: controller focus navigation. Optional.

Reference them with absolute paths and never ship copies of them: OSF UI owns these files.

## What a page cannot do

Pages are sandboxed to local files. The origin is `https://osfui.example`.

- **No network access.** Remote `http(s)`, WebSockets, WebRTC, and workers are
  blocked. Bundle every asset, including fonts.
- A page can only navigate within its own view folder. Popups and new windows are
  blocked.
- `data:` and same-origin `blob:` URLs work.
- Your page runs in either Microsoft Edge WebView2 or bundled Chromium (CEF),
  depending on the player's install. Both are current Chromium, so modern JS and CSS
  work in both.

## Settings and hotkeys

OSF UI does not pass OSF Settings values to pages. To use a setting in your page:

1. Declare the setting or hotkey in an OSF Settings schema, shipped at
   `Data/SFSE/Plugins/OSF/Settings/schemas/<mod>.json`. See the OSF Settings
   [SETTINGS](https://github.com/ozooma10/osf-settings-slim/blob/main/docs/SETTINGS.md)
   and [KEYBINDINGS](https://github.com/ozooma10/osf-settings-slim/blob/main/docs/KEYBINDINGS.md)
   docs.
2. Read the value in your backend.
3. Publish the value to the page with `SetState`.

To open a view from a hotkey, register a hotkey callback with OSF Settings and call
`RequestMenu` from it. The [settings-view example](../examples/settings-view/README.md)
does exactly this.

## Developing and debugging

Turn on **Developer mode** in OSF Settings (*OSF UI → Developer*), then restart the
game. Developer mode gives you:

- **F12** opens DevTools for the open menu.
- **F11** toggles the performance overlay for the active local view. It works with
  both browser backends and v1 views, without adding a script to the mod.
- **Live reload.** Saving a file in a loaded view reloads every open view of that
  mod. Manifest changes still need a restart.
- Page `console` output and uncaught errors written to `OSF UI.log`.
- Bridge protocol errors sent to the page console (as `osfui.debug.error`).
- `debugOnly` views become active.

The logs are in `Documents\My Games\Starfield\SFSE\Logs\`:

- `OSF UI.log` records discovery, manifest errors, and bridge faults.
- `OSF UI.webview2-host.log` (or `OSF UI.cef-host.log`) is the browser process log.

To trace every bridge message in the page console, run
`localStorage["osfui:trace"] = "1"` in DevTools and reload.

For scrolling diagnostics, open the affected view, press **F11**, and scroll. The
top-right overlay shows FPS, average / p95 / worst frame intervals, counts over
33.3 and 50 ms, and a frame-time graph for the last five seconds. **Scroll FPS**
uses intervals during top-level document scrolling (including nested scrollers)
and the following 200 ms; a dash means there are no recent scroll samples. Scroll
events inside child frames are not included. The session worst frame and
JavaScript tasks lasting at least 50 ms remain visible after a hitch has passed. Long-task counts and their
total duration accumulate while sampling, including work in descendant frames
when reported by the browser.

Press **F11** again to stop sampling and write a JSON summary prefixed
`[OSF UI performance]` to the page console. Menu and HUD views also forward it to
the existing developer console log in `OSF UI.log`. Enabling the overlay starts a
fresh session. In DevTools,
`osfui.performance.snapshot()` returns the current numbers,
`osfui.performance.reset()` clears them, and `.show()` / `.hide()` also control
the overlay (including on HUD views). Sampling pauses when the view or document
is hidden; returning to it clears the recent window so hidden time is not counted
as a hitch. The overlay updates four times per second, ignores pointer input, and
does no animation sampling or long-task observation while disabled.

These are **browser `requestAnimationFrame` intervals**, not Starfield FPS, GPU
execution time, or proof that a frame reached the game. High p95 / worst values
with long tasks suggest page main-thread work worth profiling with F12 DevTools.
If scrolling looks choppy while browser timings stay smooth, investigate input
delivery, browser capture / texture transfer, and game compositing next. The
overlay itself causes small periodic paints, so disable it for baseline captures.
Remote websites do not receive this diagnostic script.

You can also open `index.html` in a normal browser for layout work. `osfui.js`
detects that there is no bridge: `send` returns `false`, `request` rejects with
`no-bridge`, and no state arrives.

## Shipping

A typical mod archive:

```text
Data/
  SFSE/Plugins/OSF/UI/views/mymod/panel/...      your views
  SFSE/Plugins/OSF/Settings/schemas/mymod.json   if you have settings or hotkeys
  Scripts/MyMod/Bridge.pex                        if you have a Papyrus backend
  MyMod.esm                                       only if you need the quest
  SFSE/Plugins/MyMod.dll                          if you have a native backend
```

List **OSF UI** and **OSF Settings** as requirements. Do not ship `views/shared/`,
`OSFUI.pex`, or `OSFUI.psc`. `OSFUI.psc` belongs on your Papyrus compiler's import
path, not in your archive.

## Troubleshooting

| Symptom | Check |
| --- | --- |
| The view never appears or isn't in the launcher | Look for `ViewManifest: [content]` in `OSF UI.log` (folder name, `manifestVersion`, `launcher` on a HUD). Did you restart after adding it? |
| `request` times out | No endpoint is registered under that name, or your handler never called `Reply`/`Reject`. For Papyrus, was registration repeated after the last load? |
| `papyrus-unavailable` | Papyrus registration was cleared by a load or main-menu return, or Papyrus is mid-load. |
| `osfui.on("itemAdded")` never fires from Papyrus | Papyrus event names are lowercased. Listen for `itemadded`. |
| State is missing after loading a save | Papyrus state is session-scoped. Publish it again from `OnPlayerLoadGame`. |
| Images or fonts don't load | They must be local files. Remote URLs are blocked. Check `OSF UI.webview2-host.log` for "blocked network egress". |
| Escape closes the menu while you need it | Send `osfui.handleBack { handle: true }` once the page loads. |

## World displays

For `kind: "world"` views, screen catalogs, generated materials, and binding APIs, see
[the optional World Screens extension](../extensions/world-screens/README.md). Menu and
HUD mods require only OSF UI; world-screen content additionally requires the extension.
