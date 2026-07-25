# Data between Papyrus and views

**Audience:** Papyrus mod authors whose view displays live game state or sends
player actions back to a script. Settings declared in a schema are covered by
[authoring-settings.md](authoring-settings.md).

The preferred API has three deliberately different concepts:

| Meaning | Papyrus | JavaScript |
|---|---|---|
| Current state | `OSFUI.SetView*` | `osfui.data.on(key, fn)` |
| One-way player action | `OnOSFUIViewAction` | `osfui.action(name, ...args)` |
| Value-returning operation | `OnOSFUIViewRequest` + `ReplyView*` | `await osfui.papyrus.request(name, ...args)` |

Use state for anything the page renders, actions for clicks that merely mutate
that state, and requests only when JavaScript genuinely needs an immediate
answer. All routes target the calling view's owning mod; a page cannot spoof a
Papyrus destination.

## Publish current state

Typed setters preserve Papyrus values as their natural JavaScript types:

```papyrus
OSFUI.SetViewBool(ModId, "enabled", enabled)
OSFUI.SetViewInt(ModId, "credits", credits)
OSFUI.SetViewFloat(ModId, "capacity", capacity)
OSFUI.SetViewString(ModId, "title", title)
OSFUI.SetViewStrings(ModId, "slots", names)
OSFUI.SetViewInts(ModId, "counts", counts)
OSFUI.SetViewFloats(ModId, "weights", weights)
OSFUI.SetViewBools(ModId, "equipped", equipped)
OSFUI.SetViewForms(ModId, "items", items)
```

Each call replaces the complete value for `(modId, key)`. OSF UI caches the
latest value for the session, sends it to every loaded view owned by the mod,
and replays it automatically when one of those views opens or reloads. There is
no page-level `ready` action and no manual resynchronization handshake.

Consume a key directly:

```js
let slots = [];
let counts = [];

osfui.data.on("slots", (value) => {
  slots = value;                 // string[]
  render();
});

osfui.data.on("counts", (value) => {
  counts = value;                // number[]
  render();
});
```

Keys are matched case-insensitively because Papyrus string interning does not
preserve authored casing. `data.on()` also caches values inside the page: a
handler registered after a message arrived receives the latest value
immediately. `osfui.data.get(key)` reads that page-local latest value without
subscribing.

The native cache is session-scoped. Re-publish authoritative state after a game
load; this is also necessary because runtime FormIDs do not survive save loads.

## Receive one-way actions

The concise registration chooses the modern argument-list shape and a fixed
callback name:

```papyrus
ScriptName AutoSortUI Extends Quest

string ModId = "yourname.autosort"
int actionToken = 0

Function RegisterUI()
    actionToken = OSFUI.ListenForViewActions(self as ScriptObject, ModId)
EndFunction

Function OnOSFUIViewAction(string action, string[] args)
    If action == "removeTag"
        int slot = args[0] as int
        RemoveTag(slot, args[1])
        OSFUI.SetViewStrings(ModId, "tags." + slot, GetSlotTags(slot))
    ElseIf action == "equip"
        Form item = OSFUI.GetFormById(args[0])
        If item != None
            EquipItem(item, args[1] as int)
        EndIf
    EndIf
EndFunction
```

The view side is intentionally small:

```js
removeButton.onclick = () => osfui.action("removeTag", slotIndex, tag);
equipButton.onclick = () => osfui.action("equip", item.formId, quantity);
```

Arguments may be strings, numbers, or booleans in JavaScript. Papyrus receives
a `string[]`; use `as int`, `as float`, and normal case-insensitive string
comparisons. A form is sent back by echoing its `formId` and resolving it with
`OSFUI.GetFormById()` or `GetFormsById()`.

Registrations are session-scoped. Register on initialization and after every
game load, and release an obsolete token with `OSFUI.Unregister(token)`.
`ListenForViewActionsStatic(script, modId)` calls the global
`OnOSFUIViewAction` function for a script library.

The older `RegisterForViewActions*` functions remain supported when a mod needs
a custom callback name or the legacy single-string argument shape.

## Ask Papyrus for a value

Use a request when returning a value is the operation itself—for example, a
calculated price or a validation result. Do not use it for ordinary buttons;
mutate state and publish the new state instead.

Register one request listener for the mod:

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

JavaScript receives the typed value directly:

```js
try {
  const price = await osfui.papyrus.request(
    "calculatePrice",
    item.formId,
    quantity,
  );
  showPrice(price);
} catch (error) {
  if (error.code === "stale-form") refreshInventory();
}
```

Available replies are `ReplyViewBool`, `ReplyViewInt`, `ReplyViewFloat`,
`ReplyViewString`, their plural array forms, and `ReplyViewForms`.
`RejectViewRequest(token, code, message)` becomes a correlated JavaScript
`Error` with `.code`.

A reply token is host-owned, one-shot, and valid for ten seconds. A duplicate,
late, unknown, or pre-load token returns `false`. Only one request listener may
own a mod id (first registration wins), and inflight requests are bounded. The
JavaScript helper waits fifteen seconds so the host's more specific
`papyrus-timeout` error wins over its local timeout.

## Real forms

`SetViewForms` serializes each form as an identity object:

```js
osfui.data.on("items", (items) => {
  // [{ formId: 1370322, formType: "WEAP", name: "...", editorId?: "..." }, null]
  renderItems(items);
});
```

A `None` or since-deleted form is `null` and keeps its array slot. A `FormList`
is one `FLST` form; push its members as a `Form[]` when the view should render
them individually. `name` is present only for forms with `TESFullName`, and
`editorId` is best-effort.

Runtime FormIDs are session-scoped. Never persist a `formId` in a save or
`localStorage`; echo it back promptly, check `GetFormById()` for `None`, and cast
to the expected type before changing game state. JavaScript never receives an
object capable of operating on the game—the script remains the authority.

For index-aligned records, publish one typed array per field:

```papyrus
OSFUI.SetViewForms(ModId, "inventory.forms", items)
OSFUI.SetViewInts(ModId, "inventory.counts", counts)
OSFUI.SetViewFloats(ModId, "inventory.weights", weights)
```

## Delivery and failure behavior

- Setters queue from the Papyrus tasklet and publish on OSF UI's next main-frame
  tick. Actions and requests queue onto the Papyrus VM. Never block waiting for
  either side to run.
- State is broadcast only to loaded views whose id begins with `<modId>/`.
  Hidden views remain live and receive updates. Unknown keys are harmless.
- Invalid mod ids and empty keys are dropped and logged. Pending pushes and
  requests are capped so a runaway script cannot grow memory without bound.
- If OSF UI is absent, natives fail soft. `OSFUI.GetVersion() == 0` is the
  installation feature check.
- Papyrus compares strings case-insensitively; JavaScript state keys are folded
  by `osfui.data` automatically. Do not create names that differ only by case.

## Legacy transient pushes

Existing mods may continue using:

```papyrus
OSFUI.PushToView(ModId, key, stringValues)
OSFUI.PushFormsToView(ModId, key, forms)
```

These produce `data.push { mod, key, values, forms? }`, are not cached by the
runtime, and therefore still require a view-fired `ready` action followed by a
fresh push. `osfui.data.on(key, fn)` can consume them and normalizes `values` or
`forms` to the handler's first argument, which makes gradual migration easy.
For new code, prefer `SetView*`.