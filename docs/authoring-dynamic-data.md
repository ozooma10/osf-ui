# Dynamic data between Papyrus and your views

**Audience:** Papyrus mod authors. If your mod ships an esm + scripts and you
want an OSF UI view to show *live* data — a list that changes at runtime, a
table, arbitrary state — this is the surface for it.

[authoring-settings.md](authoring-settings.md) covers *settings*: scalar
values in a pre-declared schema. This page covers everything that doesn't fit
that shape.

The whole API is three functions on the shipped `OSFUI` script (plus the
`Unregister` you already know):

```papyrus
Function PushToView(string asModId, string asKey, string[] asValues) Global Native
; scalar-arg callback: OnUIAction(string asAction, string asArg)
int Function RegisterForViewActions(ScriptObject akReceiver, string asFn, string asModId) Global Native
int Function RegisterForViewActionsStatic(string asScript, string asFn, string asModId) Global Native
; args-list callback: OnUIAction(string asAction, string[] asArgs)  — host 1.3.0+
int Function RegisterForViewActionsArgs(ScriptObject akReceiver, string asFn, string asModId) Global Native
int Function RegisterForViewActionsArgsStatic(string asScript, string asFn, string asModId) Global Native
; real forms across the bridge — host 1.3+ (see "Real forms" below)
Function PushFormsToView(string asModId, string asKey, Form[] akForms) Global Native
Form Function GetFormById(string asFormId) Global Native
Form[] Function GetFormsById(string[] asFormIds) Global Native
```

## The model: your script owns the truth

Data flows one way, actions flow the other:

```
Papyrus script  ──  PushToView(...)  ──▶  your view   (data.push message)
Papyrus script  ◀──  OnUIAction(...) ──   your view   (ui.action command)
```

- Your **script pushes** state. OSF UI stores nothing - every push is
  delivered to your mod's currently-loaded views and forgotten.
- Your **view fires actions** (a click, a toggle). Actions are
  **fire-and-forget**: they have no return value and no reply. The way a view
  "gets an answer" is that your script reacts by pushing updated state.
- When a view loads (or reloads, or the player loads a save), it fires a
  `ready` action. Your script answers by pushing current state. This
  handshake is the whole synchronization story — there is no snapshot cache
  to fall out of date.

Everything is **eventually consistent**: a push is queued on your script's
thread and delivered on OSF UI's next frame; an action is queued onto the
Papyrus VM and runs when the VM gets to it. Never assume "I pushed, so the
view has rendered it" — and never wait on an action for a result.

## Worked example: porting a terminal menu

Say your container auto-sort mod tracks ~21 container slots, each with a list
of tag keywords, and today the player edits them through a terminal. Replacing
that with an OSF UI view takes three pieces.

### 1. The view folder

Ship a view under your mod's namespace folder
(see [authoring-views.md](authoring-views.md) for the full layout). OSF UI
discovers this drop-in folder at boot and loads it on the first
`menu.open`, Papyrus `OSFUI.OpenMenu`, or native `RequestMenu`; a Papyrus-only
mod needs no `config.json` edit or companion plugin. `config.json` is for the
user's boot-time composition, while native plugins may still use
`RegisterView` to load a plugin-shipped folder explicitly:

```
SFSE/Plugins/OSFUI/views/yourname.autosort/slots/
  manifest.json
  index.html
  main.js
```

```json
{
  "id": "yourname.autosort/slots",
  "mod": "yourname.autosort",
  "title": "Auto-Sort Slots",
  "kind": "menu"
}
```

Open it from a terminal replacement, an activator, or a hotkey with
`OSFUI.OpenMenu("yourname.autosort/slots")`.

### 2. The view logic (`main.js`)

```js
// Rendering only — the script owns the data. State arrives as data.push.
let slots = [];   // ["Weapons", "Aid", ...]           key "slots"
let tags = {};    // slot index -> ["WeapMelee", ...]  key "tags.<index>"

osfui.onMessage = (msg) => {
  if (msg.type !== 'data.push') return;
  const { key, values } = msg.payload;
  const k = key.toLowerCase();          // Papyrus casing is not preserved —
  if (k === 'slots') {                  // compare case-insensitively
    slots = values;
  } else if (k.startsWith('tags.')) {
    tags[Number(k.slice(5))] = values;
  } else {
    return;                             // unknown key: ignore (additive contract)
  }
  render();
};

// The handshake: ask the script to push current state. Fired on every page
// load — which is also every overlay reload and every game load resync.
osfui.send('ui.action', { action: 'ready' });

// A click fires an action; the script answers with a fresh push. Pass several
// values as a list — each becomes one asArgs[i] on the Papyrus side (host
// 1.3+); host coerces non-strings, so numbers are fine.
function onRemoveTag(slotIndex, tag) {
  osfui.send('ui.action', { action: 'removeTag', args: [slotIndex, tag] });
}
```

`args` is a LIST — send each value as its own element and index it directly on
the script side (below). Prefer this over the old idiom of packing multiple
values into one `arg` string (`"3:WeapMelee"`, or `kind*100+slot` for ints):
Papyrus has neither substring parsing nor a modulo operator, so unpacking a
blob was always the awkward part. A single scalar still works — send `arg:
"..."` and read it as the second parameter of the scalar callback form.

### 3. The Papyrus script

Register with `RegisterForViewActionsArgs` to receive the argument list as a
`string[]`. (The older `RegisterForViewActions` delivers a single `string asArg`
instead — use it only if you target hosts before 1.3.0.)

```papyrus
ScriptName AutoSortUI Extends Quest

int uiToken = 0

Function RegisterUI()
    uiToken = OSFUI.RegisterForViewActionsArgs(self as ScriptObject, "OnUIAction", "yourname.autosort")
EndFunction

Function OnUIAction(string asAction, string[] asArgs)
    If asAction == "ready"           ; view (re)opened or resynced: push everything
        PushAll()
    ElseIf asAction == "removeTag"   ; asArgs = [slotIndex, tag]
        int slot = asArgs[0] as int
        RemoveTag(slot, asArgs[1])
        PushSlot(slot)               ; the "reply" is a fresh push
    EndIf
EndFunction

Function PushAll()
    OSFUI.PushToView("yourname.autosort", "slots", GetSlotNames())
    int i = 0
    While i < GetSlotCount()
        PushSlot(i)
        i += 1
    EndWhile
EndFunction

Function PushSlot(int aiSlot)
    OSFUI.PushToView("yourname.autosort", "tags." + aiSlot, GetSlotTags(aiSlot))
EndFunction
```

That's the whole loop: `ready` → push, click → action → mutate → push.

## Contract details

- **Re-register on every game load.** Registrations are session-scoped, like
  `RegisterForSettingChanges`: they do not survive a save load. Call your
  `RegisterUI()` from quest init *and* every load (e.g. `OnPlayerLoadGame` on
  a player `ReferenceAlias`). The view side needs nothing special — its next
  `ready` (pages keep running across loads, and re-fire on reload) meets your
  re-registered handler. Release with `OSFUI.Unregister(uiToken)`.
- **Delivery targets every live view of your mod.** A push goes to each
  loaded view whose id starts with `<asModId>/` — including hidden or closed
  ones (their JS keeps running), and a view that hasn't finished loading yet
  (the message queues until the page is ready). If your mod ships several
  views, namespace your keys per view or just let each view ignore the keys
  it doesn't know.
- **String casing is not yours.** Papyrus interns strings process-wide, so
  every string that crosses this API — mod ids, keys, actions, args — may
  arrive cased differently than you or the view wrote it. In Papyrus, compare
  with `==` (already case-insensitive). In JS, fold before comparing
  (`key.toLowerCase()`), and never make two keys or actions that differ only
  by case.
- **Fails soft, logs loud.** If OSF UI is absent the natives are unbound and
  calls yield defaults (`RegisterForViewActions` → 0) — gate on
  `OSFUI.GetVersion()` like every other call. An invalid mod id or empty key
  is dropped with a WARN in `OSF UI.log`. Pushes are capped at 1024 pending
  deliveries (drop-newest, logged) — unreachable in normal use, it only guards
  a runaway push loop while the overlay is disabled.
- **Values are strings; forms are references.** `PushToView` carries
  `string[]` display data. Real game forms cross the bridge only through
  `PushFormsToView` (below) — as *references* the script resolves back, never
  as anything JS could operate on itself. The view still cannot touch game
  state; every game operation runs in your Papyrus.

## Real forms across the bridge (host 1.3+)

Before 1.3, a mod with a dynamic form list (keywords, items) had to encode
each form as a catalog index and resolve it back by position. Now the script
pushes the actual `Form[]`:

```papyrus
Keyword[] kws = GetCatalogKeywords()
OSFUI.PushFormsToView("yourname.autosort", "catalog", kws as Form[])
```

Each form arrives in the same `data.push` handler as an identity object in a
`forms` array (`values` is `[]` on a forms push):

```js
osfui.on('data.push', ({ key, values, forms }) => {
  if (key.toLowerCase() === 'catalog') {
    // forms: [{ formId: 1370322, formType: "KYWD", name: "Melee Weapons" }, ...]
    renderCatalog(forms.filter(Boolean));   // a None input arrives as null
  }
});
```

`formId` is the form's runtime FormID — and it is also how the view refers to
the form later. A click just echoes it back as an ordinary args element:

```js
osfui.send('ui.action', { action: 'addTag', args: [slotIndex, form.formId] });
```

and the script resolves it with one call — no index math, no catalogs:

```papyrus
Function OnUIAction(string asAction, string[] asArgs)
    If asAction == "addTag"
        Keyword kw = OSFUI.GetFormById(asArgs[1]) as Keyword
        If kw != None                    ; ALWAYS check — see the caveats below
            AddTag(asArgs[0] as int, kw)
            PushSlot(asArgs[0] as int)
        EndIf
    EndIf
EndFunction
```

`GetFormById` accepts what a view actually sends: decimal (a JS number
crosses the bridge as `"1370322"`) or `"0x0014E8D2"` hex. Unlike
`Game.GetForm`, hex and the full 32-bit dynamic-FormID range both work.

### Pairing forms with display data

The serialized objects carry identity only (`formId`, `formType`, `name`,
best-effort `editorId`). Anything else — counts, equipped state, prices —
you push alongside as normal values, **index-aligned**: a `None` (or deleted)
form keeps its slot as `null` in the `forms` array precisely so the two
arrays line up.

```papyrus
Form[] items = GetTrackedItems()
OSFUI.PushFormsToView("yourname.mod", "inv", items)
OSFUI.PushToView("yourname.mod", "invCounts", GetTrackedCounts())  ; counts[i] belongs to forms[i]
```

### Caveats

- **Runtime FormIDs are session-scoped.** Never persist one (script var
  across saves, localStorage, anywhere). Dynamic (`FF`-prefixed) ids are even
  recycled across save loads, so a stale reference can resolve to a
  *different* form. In practice the `ready` re-handshake replaces the view's
  state on every reload; still, resolve promptly, check for `None`, and cast
  to the type you expect (`as Keyword`) before doing anything destructive.
- **A FormList serializes as one form** (`formType: "FLST"`), not its
  members. Push the members as a `Form[]` (a `GetSize`/`GetAt` loop) when the
  view should see them — you usually want to control order/filtering anyway.
- `name` is the form's `TESFullName` and is omitted when it has none;
  `editorId` is usually unavailable at runtime in Starfield. For forms
  without names (e.g. bare `ObjectReference`s), push display text as a
  parallel values array.
- There is no "ask the game about this form" query from JS. The view renders
  what the script pushed; if it needs more, fire an action and let the script
  push more.
