# Dynamic data between Papyrus and your views

**Audience:** Papyrus mod authors. If your mod ships an esm + scripts and you
want an OSF UI view to show *live* data — a list that changes at runtime, a
table, arbitrary state — this is the surface for it. No SFSE plugin, no C++.

[authoring-settings.md](authoring-settings.md) covers *settings*: scalar
values in a pre-declared schema. This page covers everything that doesn't fit
that shape.

The whole API is three functions on the shipped `OSFUI` script (plus the
`Unregister` you already know):

```papyrus
Function PushToView(string asModId, string asKey, string[] asValues) Global Native
int Function RegisterForViewActions(ScriptObject akReceiver, string asFn, string asModId) Global Native
int Function RegisterForViewActionsStatic(string asScript, string asFn, string asModId) Global Native
```

## The model: your script owns the truth

Data flows one way, actions flow the other:

```
Papyrus script  ──  PushToView(...)  ──▶  your view   (data.push message)
Papyrus script  ◀──  OnUIAction(...) ──   your view   (ui.action command)
```

- Your **script pushes** state. OSF UI stores nothing — every push is
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
(see [authoring-views.md](authoring-views.md) for the full layout and how the
view gets loaded — a `config.json` `views` entry, or a companion plugin's
`RegisterView`):

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

// A click fires an action; the script answers with a fresh push.
function onRemoveTag(slotIndex, tag) {
  osfui.send('ui.action', { action: 'removeTag', arg: slotIndex + ':' + tag });
}
```

`arg` is a single string — encode structure yourself (`"3:WeapMelee"` above).
For anything bigger, prefer more actions with small args over parsing blobs.

### 3. The Papyrus script

```papyrus
ScriptName AutoSortUI Extends Quest

int uiToken = 0

Function RegisterUI()
    uiToken = OSFUI.RegisterForViewActions(self as ScriptObject, "OnUIAction", "yourname.autosort")
EndFunction

Function OnUIAction(string asAction, string asArg)
    If asAction == "ready"          ; view (re)opened or resynced: push everything
        PushAll()
    ElseIf asAction == "removeTag"  ; "<slotIndex>:<tag>"
        int sep = StringFindSubstring(asArg, ":")
        int slot = StringToInt(StringSubstring(asArg, 0, sep))
        RemoveTag(slot, StringSubstring(asArg, sep + 1))
        PushSlot(slot)              ; the "reply" is a fresh push
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
- **Strings only, by design.** Values are `string[]`; there is no `Form` or
  `ObjectReference` marshaling. Push display text and stable ids you can
  resolve back on the script side, never anything you'd need the VM to look
  up from the view.
