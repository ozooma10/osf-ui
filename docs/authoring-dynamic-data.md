# State and events: feeding a view live game data

For mod authors whose view shows live game state or sends player actions back to a script or plugin. Schema-declared settings are [authoring-settings.md](authoring-settings.md); the view-side verbs are [authoring-views.md](authoring-views.md) §3.

Your backend owns the game. The view is a **reload-prone document**: recreated on F5, on a dev hot-reload, after a crash-recovery reload, and when OSF UI reclaims an idle view the player then reopens. Every rule below follows from that.

## One decision, and it is the whole design

Before you publish anything: **is this true right now, or did it just happen?**

| | means | delivered to | on the next reload |
|---|---|---|---|
| **state** | what is TRUE NOW — a list, count, status, selection | every live view of your mod, and every one that loads later | **replayed automatically**, before the page's first event |
| **event** | what JUST HAPPENED — a scan finished, the player took a hit | every live view of your mod, once | **never** |

Backwards, you get one of two classic bugs:

- **An event encoded as state re-fires** — replayed to every fresh document, so "you took a hit" plays its sound again on every reload.
- **State encoded as an event leaves a blank HUD** — the push happened once, before the reload, and nothing replays it. This is the bug 1.x asked every author to work around by hand (the view fired a `ready` action, the script re-pushed). That convention is gone, along with the `data.push` channel that needed it.

The test: **if reloading the page mid-session leaves it correct, you chose right.** In `devMode`, F5 the view and look. A correctly fed view needs *zero* lifecycle code.

## The grid

Every backend expresses the same four kinds. Gaps here are API bugs, which is why 2.0 added the two marked *new*:

| | publish **state** | announce an **event** | receive a one-way message | answer a **request** |
|---|---|---|---|---|
| **Papyrus** | `OSFUI.SetView*` | `OSFUI.SendViewEvent` *(new)* | `ListenForViewActions` → `OnOSFUIViewAction` | `ListenForViewRequests` → `OnOSFUIViewRequest` + `ReplyView*` |
| **Native plugin (ABI 2.0)** | `SetViewState` | `SendToWeb` | `RegisterSend` | `RegisterRequest` |
| **The view sees** | `osfui.state.on("<mod>/<key>", fn)` | `osfui.on("<mod>.<name>", fn)` | `osfui.papyrus.call(...)` / `send(...)` | `osfui.papyrus.request(...)` / `osfui.request(...)` |

State, events, registered listeners, and native endpoints are scoped to the calling view's **owning mod**, derived from its view id. `papyrus.call` is the explicit exception: it names an arbitrary GLOBAL script/function and therefore carries the authority of installed local mod content.

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

Each call replaces the **complete** value for `(modId, key)` — state is never a delta, so a replay and a live update are the same message. OSF UI retains the latest value, sends it to every live view owned by the mod, and replays it to any view of that mod that greets later.

```papyrus
ScriptName AutoSortUI Extends Quest

string ModId = "yourname.autosort"
int actionToken = 0

; Call from OnInit AND from load-game handling: OSF UI registrations are
; session-scoped, like every Papyrus event registration.
Function RegisterUI()
    actionToken = OSFUI.ListenForViewActions(self as ScriptObject, ModId)
    PublishAll()
EndFunction

; The one publish path. Call after every change and every game load — nothing
; else in the mod, and nothing in the view, has to know a page reloaded.
Function PublishAll()
    OSFUI.SetViewStrings(ModId, "slots", GetSlotNames())
    OSFUI.SetViewInts(ModId, "counts", GetSlotCounts())
EndFunction
```

The retained cache is **session-scoped for Papyrus**: dropped on a game load, because a Papyrus value may hold form identities and runtime FormIDs don't survive a load. Re-publish where you re-register.

## Consume state in the view

```js
const MOD = "yourname.autosort";

let slots = [];
let counts = [];

osfui.state.on(`${MOD}/slots`, (value) => { slots = value; render(); });
osfui.state.on(`${MOD}/counts`, (value) => { counts = value; render(); });
```

That's the entire lifecycle. `state.on()` replays the current value **synchronously** at subscribe time if one has arrived, and fires again on every change — including on the fresh document after a reload, because the replay precedes your first event. No readiness check, no `ready` action, nothing to redo in a visibility handler.

- A state key is `"<modId>/<key>"`. Write it out, or build it from `(await osfui.ready).mod`.
- Keys match **case-insensitively** on both halves. Papyrus string interning hands back the first casing the process saw, not what your script spelled, so the fold isn't optional. Don't create two keys differing only by case.
- `osfui.state.get(key)` reads the latest value without subscribing — an escape hatch, not the normal path.
- The value arrives **verbatim**: a number is a number, a `SetViewStrings` list is a `string[]`, a `SetViewForms` list is an array of objects.

## Publish state from a native plugin

Before 2.0, state was Papyrus-only, so a plugin hand-rolled reload handling: invent a "the page reloaded" message, have the view send it, re-push everything. `SetViewState` closes that gap.

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
    // Addressed to your MOD, not a view: every live view of acme.scanner gets it
    // now, and every one that loads later gets it on its handshake.
    (void)json.SetViewState("acme.scanner", "contacts", value);
}
```

The view reads it exactly like Papyrus state: `osfui.state.on("acme.scanner/contacts", render)`.

- Any JSON **value** is legal, not just an object. The raw C ABI takes JSON text (`const char*` is the only shape that survives the vtable contract); `OSFUI_JSON.h`'s `JsonClient` adds the `nlohmann::json` overloads so you never call `dump()`.
- Thread-safe. Validation is synchronous (returns `false` on a bad mod id, an empty or over-128-character key, or unparsable JSON); delivery applies on the next main tick.
- **Not** session-scoped: plugin state survives a game load. A plugin's HUD configuration holds no form identities, and wiping it every load would be the bug. (Papyrus state is wiped, for the opposite reason.)

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

`payload.args` is always an array of strings (empty when none). Nothing is cached: a view that opens afterwards never sees it. If a late-opening view *should* see it, what you have is state — and the two coexist happily ("publish the new count as state, announce the moment as an event").

`SendViewEvent` deliberately rejects forms. Publish the form through `SetViewForms` and announce the change with an event naming its key; an event carrying a form identity would be an identity nobody is allowed to keep.

## Announce an event from a native plugin

```cpp
OSFUI::API::JsonClient json{ g_ui };
(void)json.SendToWeb("acme.scanner/hud", "acme.scanner.contactLost",
                     nlohmann::json{ { "id", 7 } });
```

The asymmetry is deliberate: `SetViewState` addresses your **mod** (everyone now and later), `SendToWeb` addresses **one view id** — an event has a specific audience and moment. Name events `<yourModId>.<name>`, matching the Papyrus channel; the platform's own names are undotted or `osfui.*`, so staying inside your mod id keeps you from shadowing one.

If the target view is known but not live (lazy, or idle-reclaimed), the message is held in a bounded per-view queue and flushed when that document greets — the ABI's message-before-first-paint guarantee, which is what lets a plugin open a view already in a specific state. The queue drops the *oldest* on overflow.

---

## Call a GLOBAL Papyrus function

A recordless view can call any GLOBAL function on any loose PEX directly. No manifest target, ESM, quest, alias, or registration is involved:

```papyrus
ScriptName AcmeScannerUI Hidden

Function RemoveTag(int slot, string tag) Global
    ; mutate game state, then publish what changed
    OSFUI.SetViewStrings("acme.scanner", "tags." + slot, GetSlotTags(slot))
EndFunction

Function Equip(string formId, int quantity, bool silent) Global
    Form item = OSFUI.GetFormById(formId)
    If item != None
        Game.GetPlayer().EquipItem(item, quantity, silent)
    EndIf
EndFunction
```

```js
removeButton.onclick = () => osfui.papyrus.call("AcmeScannerUI", "RemoveTag", slotIndex, tag);
equipButton.onclick  = () => osfui.papyrus.call("AcmeScannerUI", "Equip", item.formId, quantity, true);
```

- JavaScript integers map to Papyrus `int`; fractional numbers map to `float`; strings and booleans retain their types. JSON erases the distinction between `3` and `3.0`, so use `osfui.papyrus.float(3)` for a whole-valued `float` parameter. Other objects, `null`, arrays, out-of-range integers, and more than 32 arguments are refused.
- The declared Papyrus signature must match exactly. Forms are not direct bridge arguments; pass a serialized `formId` string and resolve it with `OSFUI.GetFormById`.
- The call is fire-and-forget: its Papyrus return value is not delivered to JavaScript. Publish observable results with `OSFUI.SetView*` or `SendViewEvent`.
- The script and function come from JavaScript. Use this only in installed local views you trust; `permissions.nativeBridge:false` removes the capability with the rest of the bridge.

Existing instance-backed mods can keep the narrower `osfui.papyrus.send(name, ...args)` listener path. `ListenForViewActions(self, modId)` or `ListenForViewActionsStatic(script, modId)` receives it as `OnOSFUIViewAction(string name, string[] args)`.

Manual registrations are session-scoped. Register on init and after every game load, and release an obsolete token with `OSFUI.Unregister(token)`.

## Ask Papyrus for a value

Use a request only when *returning a value is the operation* — a calculated price, a validation result. For an ordinary button, mutate and publish state; a request that only triggers a mutation is a round trip you pay for on every click.

```papyrus
Function OnOSFUIViewRequest(string request, string[] args, string replyToken) Global
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

The helper resolves with the typed value the script replied and rejects with an `Error` carrying the script's own `code`. Replies: `ReplyViewBool`, `ReplyViewInt`, `ReplyViewFloat`, `ReplyViewString`, their plural array forms, and `ReplyViewForms`. Register an instance with `ListenForViewRequests(self, modId)` or a GLOBAL library with `ListenForViewRequestsStatic(script, modId)` before requests can reach it.

A reply token is host-owned, one-shot, valid for ten seconds; a duplicate, late, unknown or pre-load token returns `false`. Answer exactly once. Only one registered request listener may own a mod id (first registration wins). In-flight requests are bounded (32 per view, 256 overall) — past that a request is refused, not queued. A backend that never answers produces a `papyrus-timeout` rejection at ten seconds; the JavaScript helper waits fifteen so the host's more specific error wins over its generic `timeout`.

---

## Real forms

`SetViewForms` serializes each form as an identity object:

```js
osfui.state.on(`${MOD}/items`, (items) => {
  // [{ formId: 1370322, formType: "WEAP", name: "…", editorId?: "…" }, null, …]
  renderItems(items);
});
```

A `None` or since-deleted form is `null` and **keeps its array slot**, so a parallel typed array stays index-aligned. `name` is present only for forms with `TESFullName`; `editorId` is best-effort (usually unavailable at runtime in Starfield); `formType` is the record signature (`"WEAP"`, `"KYWD"`, `"FLST"`, …), falling back to the numeric enum for an unknown type. A `FormList` is ONE form (`"FLST"`) — publish its members as a `Form[]` when the view should render them individually.

For index-aligned records, publish one typed array per field:

```papyrus
OSFUI.SetViewForms(ModId, "inventory.forms", items)
OSFUI.SetViewInts(ModId, "inventory.counts", counts)
OSFUI.SetViewFloats(ModId, "inventory.weights", weights)
```

Runtime FormIDs are session-scoped. Never persist a `formId` in a save or `localStorage`; echo it back promptly, check `OSFUI.GetFormById()` for `None`, and cast to the expected type before changing game state (`GetFormById(args[0]) as Keyword`). JavaScript never receives an object capable of operating on the game — the script stays the authority, and a stale id resolves to `None` rather than to somebody else's form.

## Delivery and failure behavior

- **Threading.** Papyrus setters and events queue from the VM tasklet and publish on the next main-frame tick; forms serialize at drain time on the main thread, because form field reads may only happen there. Direct GLOBAL calls, actions, and requests queue onto the Papyrus VM. Native `SetViewState` / `SendToWeb` are callable from any thread and applied next tick. Never block waiting for the other side.
- **Targets.** State and events reach loaded views whose id begins with `<modId>/`. A hidden suspended view resumes to receive an update. A publish with no live view is not a lost write *for state* (retained and replayed to the mod's first view) but **is** a dropped event, logged in `devMode`.
- **Bounds.** At most 64 retained state keys per mod (updating an already-retained key always works, so a fixed key set is never affected); pending Papyrus state and event queues cap at 1024 each; a plugin's pending state ops cap at 256. Invalid mod ids and empty keys are dropped and logged. These stop a runaway script growing the process without bound.
- **Absence.** If OSF UI isn't installed, every native fails soft; `OSFUI.GetVersion() == 0` is the feature check. Natively, `OSFUI_RequestBridge` returns `nullptr` and every `Client` method degrades to `false`/`0`/no-op.
- **Debugging.** F12 DevTools on the view (devMode) plus `localStorage["osfui:trace"] = "1"` and a reload: every replayed `state` envelope is visible at document boot, which answers "why is my HUD blank" in one look — either the key arrives (view bug) or it doesn't (backend bug).

## Coming from 1.x

| 1.x | 2.0 |
|---|---|
| `OSFUI.PushToView` / `PushFormsToView` | **removed.** `SetView*` for state, `SendViewEvent` for happenings |
| the view's `ready` action + the script's re-push | **removed.** State replays on every document; delete both halves |
| `data.push` / `data.state` messages | one `state` envelope, consumed with `osfui.state.on()` |
| `osfui.data.on(key, fn)` / `osfui.data.get(key)` | `osfui.state.on("<mod>/<key>", fn)` / `osfui.state.get(...)` |
| `osfui.action(name, ...args)` | `osfui.papyrus.send(name, ...args)` |
| `RegisterForViewActions{,Static,Args,ArgsStatic}` | `ListenForViewActions{,Static}` → `OnOSFUIViewAction(string, string[])` |
| a plugin's hand-rolled "I reloaded" message | `SetViewState` — the platform owns the replay |

`PushToView` was transient like an event but shaped like state, which is why every mod using it needed the `ready` handshake. Splitting the concepts is what let that convention — and the blank-after-F5 bug class — be deleted rather than documented.

Papyrus keeps its 1.5 names elsewhere on purpose: `ListenForViewActions`, `OnOSFUIViewAction`, `ListenForViewRequests` and the `ReplyView*` family are unchanged, because renaming them would churn exactly the mods that already migrated, in the one language where migrating means recompiling `.pex` files.
