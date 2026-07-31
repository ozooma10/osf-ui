# State and events: feeding a view live game data

**Audience:** mod authors whose view shows live game state, or sends player
actions back to a script or plugin. Settings declared in a schema are covered
by [authoring-settings.md](authoring-settings.md); the view-side verbs
themselves are [authoring-views.md](authoring-views.md) §3.

Your backend owns the game. The view is a **reload-prone document**: it is
recreated on F5, on a dev hot-reload, after a crash-recovery reload, and when
OSF UI reclaims an idle view and the player opens it again. Every rule below
exists because of that one fact.

## One decision, and it is the whole design

Before you publish anything, answer one question about the value: **is this
something that is true right now, or something that just happened?**

| | means | delivered to | on the next reload |
|---|---|---|---|
| **state** | what is TRUE NOW — a list, a count, a status, a selection | every live view of your mod, and every view of your mod that loads later | **replayed automatically**, before the page's first event |
| **event** | what JUST HAPPENED — a scan finished, the player took a hit, a door opened | every live view of your mod, once | **never** — a document that was not open for it never learns about it |

Get it backwards and you get one of two classic bugs, in both directions:

- **An event encoded as state re-fires.** The value is replayed to every fresh
  document, so "you took a hit" plays its sound again every time the page
  reloads.
- **State encoded as an event leaves a blank HUD.** The push happened once,
  before the reload, and nothing replays it. This is the bug that 1.x asked
  every author to work around by hand — the view fired a `ready` action on
  load and the script re-pushed everything behind it. That convention is gone,
  and so is the `data.push` channel that needed it.

The practical test: **if reloading the page mid-session leaves it correct, you
chose right.** Test it — in `devMode`, F5 the view (or edit a file and let it
auto-reload) and look at the result. A correctly fed view needs *zero*
lifecycle code.

## The grid

Every backend expresses the same four kinds. Gaps in this grid are API bugs,
which is why 2.0 added the two entries marked *new*:

| | publish **state** | announce an **event** | receive a one-way message | answer a **request** |
|---|---|---|---|---|
| **Papyrus** | `OSFUI.SetView*` | `OSFUI.SendViewEvent` *(new)* | `ListenForViewActions` → `OnOSFUIViewAction` | `ListenForViewRequests` → `OnOSFUIViewRequest` + `ReplyView*` |
| **Native plugin (ABI 2.0)** | `SetViewState` *(new)* | `SendToWeb` | `RegisterCommand` | `RegisterRequest` |
| **The view sees** | `osfui.state.on("<mod>/<key>", fn)` | `osfui.on("<mod>.<name>", fn)` | `osfui.papyrus.send(...)` / `osfui.send(...)` | `osfui.papyrus.request(...)` / `osfui.request(...)` |

All routes are scoped to the calling view's **owning mod**, derived from its
view id. A page cannot name a different mod's script or a different mod's
state, and no payload field can change that.

---

## Publish state from Papyrus

Typed setters preserve Papyrus values as their natural JavaScript types:

```papyrus
OSFUI.SetViewBool(ModId, "enabled", enabled)
OSFUI.SetViewInt(ModId, "credits", credits)
OSFUI.SetViewFloat(ModId, "capacity", capacity)
OSFUI.SetViewString(ModId, "title", title)
OSFUI.SetViewBools(ModId, "equipped", equipped)
OSFUI.SetViewInts(ModId, "counts", counts)
OSFUI.SetViewFloats(ModId, "weights", weights)
OSFUI.SetViewStrings(ModId, "slots", names)
OSFUI.SetViewForms(ModId, "items", items)
```

Each call replaces the **complete** value for `(modId, key)` — state is never a
delta, so a replay and a live update are the same message and there is no
"apply the patch" path to get wrong. OSF UI retains the latest value, sends it
to every live view owned by the mod, and replays it to any view of that mod
that greets the bridge later.

A short, complete script:

```papyrus
ScriptName AutoSortUI Extends Quest

string ModId = "yourname.autosort"
int actionToken = 0

; Call from OnInit AND from your load-game handling: OSF UI registrations are
; session-scoped, like every other Papyrus event registration.
Function RegisterUI()
    actionToken = OSFUI.ListenForViewActions(self as ScriptObject, ModId)
    PublishAll()
EndFunction

; The one publish path. Call it after every change and after every game load —
; nothing else in the mod, and nothing in the view, has to know when a page
; reloaded.
Function PublishAll()
    OSFUI.SetViewStrings(ModId, "slots", GetSlotNames())
    OSFUI.SetViewInts(ModId, "counts", GetSlotCounts())
EndFunction
```

The retained cache is **session-scoped for Papyrus**: it is dropped on a game
load, because a Papyrus value may hold form identities and runtime FormIDs do
not survive a load. Re-publish authoritative state when your script handles the
load, in the same place you re-register.

## Consume state in the view

```js
const MOD = "yourname.autosort";

let slots = [];
let counts = [];

osfui.state.on(`${MOD}/slots`, (value) => { slots = value; render(); });
osfui.state.on(`${MOD}/counts`, (value) => { counts = value; render(); });
```

That is the entire lifecycle. `state.on()` replays the current value
**synchronously** at subscribe time if one has already arrived, and fires again
on every change — including on the fresh document after a reload, because the
replay happens before your first event. There is no readiness check to write,
no `ready` action to fire, and nothing to redo in a visibility handler.

- A state key is `"<modId>/<key>"`. Write it out, or build it from
  `(await osfui.ready).mod` if you would rather not hardcode your own id.
- Keys are matched **case-insensitively** on both halves. Papyrus string
  interning hands back the first casing the process saw, not the casing your
  script spelled, so the fold is not optional. Don't create two keys that
  differ only by case.
- `osfui.state.get(key)` reads the latest value without subscribing — an
  imperative escape hatch, not the normal path.
- The value arrives **verbatim**: a number is a number, a `SetViewStrings` list
  is a `string[]`, a `SetViewForms` list is an array of objects. (1.x inspected
  each push for `value` / `values` / `forms` and handed you whichever it found;
  2.0 carries one opaque value beside the routing, so every shape travels the
  same path.)

## Publish state from a native plugin

Before 2.0 state was Papyrus-only, so a plugin had to hand-roll reload
handling: invent a "the page reloaded" message, have the view send it, and
re-push everything. `SetViewState` closes that gap — the plugin sets a value
once and the platform owns the replay.

```cpp
#include "OSFUI_API.h"
#include "OSFUI_JSON.h"   // optional; nlohmann overloads, header-only

static OSFUI::API::Client g_ui;   // static/leaked: handlers may fire for process life

void OnContactsChanged(const std::vector<Contact>& a_contacts)
{
    OSFUI::API::JsonClient json{ g_ui };
    nlohmann::json value = nlohmann::json::array();
    for (const auto& c : a_contacts) {
        value.push_back({ { "name", c.name }, { "distance", c.distance } });
    }
    // Addressed to your MOD, not to a view: every live view of acme.scanner
    // gets it now, and every one that loads later gets it on its handshake.
    (void)json.SetViewState("acme.scanner", "contacts", value);
}
```

The view reads it exactly like Papyrus state: `osfui.state.on("acme.scanner/contacts", render)`.

Details worth knowing:

- Any JSON **value** is legal, not just an object — a number or an array is a
  perfectly good state key. The raw C ABI takes JSON text (`const char*` is the
  only shape that survives the vtable contract); `OSFUI_JSON.h`'s `JsonClient`
  adds the `nlohmann::json` overloads so you never call `dump()` yourself.
- Thread-safe; validation is synchronous (the call returns `false` on a bad mod
  id, an empty or over-128-character key, or unparsable JSON), and delivery is
  applied on the next main tick.
- **Not** session-scoped: plugin state survives a game load. A plugin's HUD
  configuration holds no form identities, and wiping it on every load would be
  the bug. (Papyrus state is wiped, for the opposite reason.)

## Announce an event from Papyrus

```papyrus
string[] args = new string[2]
args[0] = "airlock-3"
args[1] = "12"
OSFUI.SendViewEvent(ModId, "scanComplete", args)

; No arguments is a zero-length array, not None:
OSFUI.SendViewEvent(ModId, "alarmCleared", new string[0])
```

The page receives it as the event `"<modId>.<name>"`:

```js
osfui.on(`${MOD}.scanComplete`, ({ args }) => {
  playChime();                    // safe: this can never re-fire on a reload
  highlight(args[0], args[1] | 0);
});
```

`payload.args` is always an array of strings (empty when the event carried
none). Nothing is cached: a view that opens afterwards never sees it. That is
the whole point — if a late-opening view *should* still see it, the thing you
have is state, and the two can happily coexist ("publish the new count as
state, announce the moment as an event").

Forms are deliberately not accepted by `SendViewEvent`. Publish the form
through `SetViewForms` and announce the change with an event naming its key;
an event that carried a form identity would be an identity nobody is allowed
to keep.

## Announce an event from a native plugin

```cpp
OSFUI::API::JsonClient json{ g_ui };
(void)json.SendToWeb("acme.scanner/hud", "acme.scanner.contactLost",
                     nlohmann::json{ { "id", 7 } });
```

Note the asymmetry, which is deliberate: `SetViewState` addresses your **mod**
(everyone now and later), while `SendToWeb` addresses **one view id** — an
event has a specific audience and a specific moment. Name the event
`<yourModId>.<name>`, matching the Papyrus channel: the platform's own event
names are undotted or `osfui.*`, and staying inside your mod id keeps you from
shadowing one.

If the target view is known but not live yet (lazy, or idle-reclaimed), the
message is held in a bounded per-view queue and flushed the moment that
document greets the bridge — this is the ABI's message-before-first-paint
guarantee, and it is what lets a plugin open a view already in a specific
state. The queue drops the *oldest* on overflow: the newest happenings are the
ones still worth delivering.

---

## One-way messages from the view

A click that only mutates game state is a `send`, not a request. Register one
listener per script; several scripts may listen for the same mod.

```papyrus
; The parameter must NOT be named "action" — that is the Action form type in
; Papyrus, and the compiler rejects references to it as a variable.
Function OnOSFUIViewAction(string actionName, string[] args)
    If actionName == "removeTag"
        int slot = args[0] as int
        RemoveTag(slot, args[1])
        OSFUI.SetViewStrings(ModId, "tags." + slot, GetSlotTags(slot))   ; publish what changed
    ElseIf actionName == "equip"
        Form item = OSFUI.GetFormById(args[0])
        If item != None
            EquipItem(item, args[1] as int)
        EndIf
    EndIf
EndFunction
```

```js
removeButton.onclick = () => osfui.papyrus.send("removeTag", slotIndex, tag);
equipButton.onclick  = () => osfui.papyrus.send("equip", item.formId, quantity);
```

- Arguments may be strings, numbers, or booleans in JavaScript; Papyrus always
  receives `string[]`. Read them with `args[i] as int` / `as float`. (The list
  form exists because Papyrus has no modulo operator, which made packing
  several small ints into one string genuinely painful.)
- `args` is never `None` — it is empty for a message sent with no arguments.
- Strings may arrive cased differently than the view sent them (interning
  again). Papyrus `==` is itself case-insensitive, so plain compares work; keep
  any case-*sensitive* comparison out of your JavaScript.
- Fire-and-forget in both directions: there is no return value and no
  acknowledgement, by design. Wanting one means it is a request. The normal
  answer to "how does the view learn what changed?" is the last line of the
  handler above — publish the new state.
- `ListenForViewActionsStatic(script, modId)` calls the GLOBAL
  `OnOSFUIViewAction` function on that script, for script libraries with no
  instance.

Registrations are session-scoped. Register on initialization and again after
every game load, and release an obsolete token with `OSFUI.Unregister(token)`.

## Ask Papyrus for a value

Use a request only when *returning a value is the operation itself* — a
calculated price, a validation result. For an ordinary button, mutate and
publish state instead; a request that exists only to trigger a mutation is a
round trip you pay for on every click.

```papyrus
int requestToken = OSFUI.ListenForViewRequests(self as ScriptObject, ModId)

Function OnOSFUIViewRequest(string request, string[] args, string replyToken)
    If request == "calculatePrice"
        Form item = OSFUI.GetFormById(args[0])
        If item == None
            OSFUI.RejectViewRequest(replyToken, "stale-form", "The item no longer exists")
            Return
        EndIf
        OSFUI.ReplyViewFloat(replyToken, CalculatePrice(item, args[1] as int))
    Else
        OSFUI.RejectViewRequest(replyToken, "unknown-request", request)
    EndIf
EndFunction
```

```js
try {
  const price = await osfui.papyrus.request("calculatePrice", item.formId, quantity);
  showPrice(price);
} catch (err) {
  if (err.code === "stale-form") refreshInventory();
}
```

The helper resolves with the typed value the script replied, and rejects with
an `Error` carrying the script's own `code`. Available replies are
`ReplyViewBool`, `ReplyViewInt`, `ReplyViewFloat`, `ReplyViewString`, their
plural array forms, and `ReplyViewForms`.

A reply token is host-owned, one-shot, and valid for ten seconds; a duplicate,
late, unknown or pre-load token returns `false`. Answer exactly once. Only one
request listener may own a mod id (first registration wins), and in-flight
requests are bounded (32 per view, 256 overall) — past that, the request is
refused rather than queued. A listener that never answers produces a
`papyrus-timeout` rejection at the ten-second mark; the JavaScript helper waits
fifteen seconds precisely so that the host's more specific error wins over its
own generic `timeout`.

---

## Real forms

`SetViewForms` serializes each form as an identity object:

```js
osfui.state.on(`${MOD}/items`, (items) => {
  // [{ formId: 1370322, formType: "WEAP", name: "…", editorId?: "…" }, null, …]
  renderItems(items);
});
```

A `None` or since-deleted form is `null` and **keeps its array slot**, so a
parallel typed array stays index-aligned. `name` is present only for forms with
`TESFullName`; `editorId` is best-effort (usually unavailable at runtime in
Starfield); `formType` is the record signature (`"WEAP"`, `"KYWD"`, `"FLST"`,
…), falling back to the numeric enum value for a type this host has no
signature for. A `FormList` is ONE form (`"FLST"`) — publish its members as a
`Form[]` when the view should render them individually.

For index-aligned records, publish one typed array per field:

```papyrus
OSFUI.SetViewForms(ModId, "inventory.forms", items)
OSFUI.SetViewInts(ModId, "inventory.counts", counts)
OSFUI.SetViewFloats(ModId, "inventory.weights", weights)
```

Runtime FormIDs are session-scoped. Never persist a `formId` in a save or in
`localStorage`; echo it back promptly, check `OSFUI.GetFormById()` for `None`,
and cast to the expected type before changing game state
(`GetFormById(args[0]) as Keyword`). JavaScript never receives an object
capable of operating on the game — the script remains the authority, and a
stale id resolves to `None` rather than to somebody else's form.

## Delivery and failure behavior

- **Threading.** Papyrus setters and events queue from the VM tasklet and are
  published on OSF UI's next main-frame tick; forms are serialized at drain
  time, on the main thread, because form field reads may only happen there.
  Actions and requests queue onto the Papyrus VM. Native `SetViewState` /
  `SendToWeb` are callable from any thread and applied on the next tick. Never
  block waiting for the other side to run.
- **Targets.** State and events reach loaded views whose id begins with
  `<modId>/`. A hidden suspended view resumes to receive an update. A publish
  with no live view is not a lost write *for state* — it is retained and
  replayed to the mod's first view when it greets — but it **is** a dropped
  event, logged in `devMode`.
- **Bounds.** At most 64 retained state keys per mod (the value of an
  already-retained key always updates, so a fixed key set is never affected);
  pending Papyrus state and event queues cap at 1024 each; a plugin's pending
  state ops cap at 256. Invalid mod ids and empty keys are dropped and logged.
  These exist so a runaway script cannot grow the process without bound.
- **Absence.** If OSF UI is not installed, every native fails soft;
  `OSFUI.GetVersion() == 0` is the feature check. On the native side,
  `OSFUI_RequestBridge` returns `nullptr` and every `Client` method degrades to
  `false`/`0`/no-op.
- **Debugging.** Open F12 DevTools on the view (devMode) and set
  `localStorage["osfui:trace"] = "1"`, then reload. Every replayed `state`
  envelope is visible at document boot, which answers "why is my HUD blank" in
  one look: either the key arrives (view bug) or it does not (backend bug).

## Coming from 1.x

| 1.x | 2.0 |
|---|---|
| `OSFUI.PushToView` / `PushFormsToView` | **removed.** Use `SetView*` for state, `SendViewEvent` for happenings |
| the view's `ready` action + the script's re-push | **removed.** State replays on every document; delete both halves |
| `data.push` / `data.state` messages | one `state` envelope, consumed with `osfui.state.on()` |
| `osfui.data.on(key, fn)` / `osfui.data.get(key)` | `osfui.state.on("<mod>/<key>", fn)` / `osfui.state.get(...)` |
| `osfui.action(name, ...args)` | `osfui.papyrus.send(name, ...args)` |
| `RegisterForViewActions{,Static,Args,ArgsStatic}` | `ListenForViewActions{,Static}` → `OnOSFUIViewAction(string, string[])` |
| a native plugin's hand-rolled "I reloaded" message | `SetViewState` — the platform owns the replay |

`PushToView` was transient like an event but shaped like state, which is why
every mod using it needed the `ready` handshake to paper over reloads. Splitting
the two concepts is what let that convention — and the whole class of
blank-after-F5 bugs — be deleted rather than documented.

Papyrus keeps its 1.5 names elsewhere on purpose: `ListenForViewActions`,
`OnOSFUIViewAction`, `ListenForViewRequests` and the `ReplyView*` family are
unchanged, because renaming them would churn exactly the mods that already
migrated, in the one language where migrating means recompiling `.pex` files.
