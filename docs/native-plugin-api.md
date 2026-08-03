# Native plugin API

Lets your DLL talk to OSF UI: handle messages a view sends, publish state and events back, read settings and hotkeys, open views, report health.

The stable, dependency-free ABI is [`sdk/OSFUI_API.h`](../sdk/OSFUI_API.h) (C ABI **1.8**). If your plugin already uses `nlohmann::json`, include the optional [`sdk/OSFUI_JSON.h`](../sdk/OSFUI_JSON.h) authoring facade too — it stays on your side of the DLL boundary and never changes the ABI.

Writing a view (HTML/JS) instead? [authoring-views.md](authoring-views.md) is the `window.osfui` side. Your `SetViewState` arrives at the view's `osfui.state.on`, your `SendToWeb` at its `osfui.on`, and its `osfui.send` / `osfui.request` arrive at your handlers.

**Contents**

- [0. When you need this](#0-when-you-need-this)
- [1. Compatibility with 1.x](#1-compatibility-with-1x)
- [2. Get the bridge](#2-get-the-bridge)
- [3. Versioning](#3-versioning)
- [4. Commands — web → native](#4-commands--web--native)
- [5. Requests — web → native, settled once](#5-requests--web--native-settled-once)
- [6. Native → web — state and events](#6-native--web--state-and-events)
- [7. Status & readiness](#7-status--readiness)
- [8. Settings, hotkeys, views, and health](#8-settings-hotkeys-views-and-health)
- [9. Threading & lifetime](#9-threading--lifetime)
- [10. Method reference](#10-method-reference)
- [11. Example plugin](#11-example-plugin)
- [12. See also](#12-see-also)

---

## 0. When you need this

Most mods need no native code:

- Ship a view: drop a folder in `views/<modId>/<viewName>/` ([authoring-views.md](authoring-views.md)).
- Ship settings: drop a schema in `settings/<author>.<modname>.json` ([authoring-settings.md](authoring-settings.md)).
- A view reads/writes its own settings and reacts to hotkeys from JS.
- Papyrus can publish view state, send view events and answer view requests with no DLL ([authoring-dynamic-data.md](authoring-dynamic-data.md)).

Use this API when your logic is in a native DLL and needs to handle view messages (and answer the ones needing an answer), publish game state or push a one-shot happening, read settings or react to changes from C++, react to a hotkey, register a schema or view folder at runtime, open or close a view, or report a durable failure into System Health.

If OSF UI is missing, every call is a safe no-op — you never special-case "not installed". Plugins compiled against any 1.x minor still connect to ABI 1.8.

---

## 1. Compatibility with 1.x

`kBridgeAPIVersion` is `(1 << 16) | 8`. The ABI stays additive: every new virtual method is appended after the complete older interface, and `Client` checks the host minor before calling it. A plugin compiled against 1.0–1.7 receives the bridge and keeps the same vtable slots.

ABI 1.8 adds one method, `SetViewState`, at the vtable tail. Code compiled with the 1.8 header detects it via `Feature::kViewState`; the `Client` wrapper does that automatically and returns `false` against a 1.0–1.7 host.

`RegisterCommand` preserves its established behavior. A view may call it with `send()` for a one-way notification. If a view calls it with `request()`, the host injects `"requestId"` into the callback payload and returns an automatic `{ ok: true, command }` reply after the handler returns — that reply means only that the handler ran. New endpoints with a meaningful result should use `RegisterRequest` and answer with `Request::Respond` / `Request::Reject`.

The web bridge protocol is independently versioned at 2.0. Native ABI compatibility does not restore removed 1.x JavaScript helper aliases or the old wire envelope; views still follow the [migration guide](mod-api-2.0-migration.md).

---

## 2. Get the bridge

`OSFUI.dll` exports one C function, `OSFUI_RequestBridge`. Don't call it directly — use `OSFUI::API::Client`, which fetches the bridge, caches the host version once, and turns any call the host is too old for into a safe no-op.

```cpp
#include "OSFUI_API.h"

static OSFUI::API::Client g_ui;

// Call ONCE, after SFSE kPostLoad.
void OnPostLoad()
{
    if (g_ui.Init()) {            // false if OSF UI is absent or a major apart
        g_ui.RegisterCommand("acme.mymod.equip", &OnEquip, nullptr);
        g_ui.RegisterRequest("acme.mymod.getWeight", &OnGetWeight, nullptr);
        g_ui.RegisterView("acme.mymod/dashboard");
    }
}
```

`Init()` returns false when OSF UI isn't installed, isn't loaded yet, or its major differs from your header. Fetch after `kPostLoad`, once — the bridge lives for the whole process and the `static Client` caches it. Don't resolve the export per-frame.

`Client::Raw()` gives you the raw `IOSFUIBridge*`. If you call tail vmethods through it, you own the version gating.

---

## 3. Versioning

| Version | Read it with | Gates |
|---|---|---|
| **C ABI** (`1.8`) | `GetInterfaceVersion()` | which native methods exist. Packed `(major<<16)｜minor`. |
| **Plugin** (OSF UI release) | `GetPluginVersion()` | nothing — log it for support. |
| **Web protocol** (`"2.0"`) | `GetBridgeProtocolVersion()` | the JS handshake. Native code: don't parse it. |

`Feature` values are their additive ABI minors. `Client::Has(Feature)` compares the running host minor before calling a tail method: requests and diagnostics need 1.7, retained native state needs 1.8. Major mismatches are refused.

---

## 4. Commands — web → native

A view calls `osfui.send("<name>", payload)`. Your handler runs on the game main thread.

```cpp
void OnEquip(const char* command,
             const char* payloadJson,   // the caller's payload object, verbatim
             const char* sourceViewId,  // who sent it
             void* user) noexcept
{
    // All const char* args are valid only during this call. Copy what you keep.
}

g_ui.RegisterCommand("acme.mymod.equip", &OnEquip, nullptr);
```

A registered command is normally a one-way `send()` endpoint. For compatibility with ABI 1.x, `request()` may also name it: the callback payload then includes the injected `requestId` and the host returns `{ ok: true, command }` after the handler returns. That auto-reply confirms dispatch, not success — use `RegisterRequest` whenever the view needs a meaningful result or failure. A `send()` naming a strict request endpoint is still dropped and surfaced as `wrong-endpoint-kind`.

**Names:** `<author>.<modname>.<name>` — a mod id (lowercase `[a-z0-9-]` segments, exactly one dot) plus a name that may contain more dots. Two dots minimum; bad shapes are refused with a log warning. Platform endpoints are dotless or single-dot, so you can't collide with them.

**Duplicates are first-wins across both registries.** A name can't be both a command and a request, and registering a name someone else owns is refused. To replace your own handler, `UnregisterCommand` then register again — the pair works back-to-back within one tick.

Register once at `kPostLoad`.

---

## 5. Requests — web → native, settled once

Use a request when the view needs an answer. It settles exactly once: a payload, a typed error, or the host deadline.

The preferred authoring form uses the optional JSON facade:

```cpp
#include "OSFUI_API.h"
#include "OSFUI_JSON.h"

static void OnGetWeight(const OSFUI::API::Request& raw, void*) noexcept
{
    OSFUI::API::JsonRequest request{ raw };
    if (!request) return;  // malformed input was already rejected

    const auto formId = request.Get<std::uint32_t>("formId");
    if (!formId) return;   // missing/wrong type was rejected as invalid-payload

    request.Respond({ { "weight", ReadWeight(*formId) } });
}

g_ui.RegisterRequest("acme.mymod.getWeight", &OnGetWeight, nullptr);
```

```js
const { weight } = await osfui.request("acme.mymod.getWeight", { formId });
```

`osfui.request()` resolves with the reply **payload** — correlation ids and envelopes are private to the bridge.

`JsonRequest::Get<T>(key)` reads a required field without throwing and rejects a missing or wrongly typed field as `invalid-payload`. `Value<T>(key, fallback)` and `TryGet()` handle optional fields; `Require<T>()` and `As<T>()` retain normal `nlohmann::json` exceptions. Malformed or non-object payloads reject as `invalid-payload`; response serialization failures as `serialization-error`. `Raw()` exposes the copyable deferred token.

`Request::Respond` also has a two-argument `(type, payload)` overload inherited from 1.7. It still compiles, but 2.0 correlates a reply by **id**, not name, so the type is transmitted nowhere — use the single-argument form.

For a fire-and-forget `RegisterCommand` callback, `JsonCommand` gives the same typed payload access plus `Command()` and `SourceViewId()`:

```cpp
void OnEquip(const char* command, const char* json, const char* source, void*) noexcept
{
    OSFUI::API::JsonCommand event{ command, json, source };
    if (!event) return;
    std::uint32_t formId{};
    if (event.TryGet("formId", formId)) Equip(formId);
}
```

**Settling.** The host owns correlation: `RequestFn` receives no `requestId`. Copy the `Request` value if you answer later; `Respond` and `Reject` are safe from any thread and settle once — a second answer is ignored and logged. The `command`, `payloadJson` and `sourceViewId` pointers are valid only during the callback.

The token is a copyable C-ABI value holding an opaque 64-bit id and host function pointers, not a host-owned object pointer. A saved copy can't dereference freed host memory: after response, timeout or view closure its id is stale and a late answer is a logged no-op.

**Failure.** `req.Reject("stable-code", "human detail")` — the view's `osfui.request()` rejects with an error carrying that `code`, and the shared helper prints it to that page's console with an `[osfui]` prefix, so your rejection reason lands in F12 DevTools with the payload attached, not only in `OSF UI.log`. Invalid response JSON rejects as `invalid-response`.

**Limits.** Requests use the same qualified grammar and first-wins namespace as commands. Each view may hold at most 64 requests in flight; overflow rejects with `request-capacity`. The fixed host deadline is 30 seconds, no opt-out, after which the view is answered `no-response`. For genuinely long work, take a *command*, publish progress and results with `SetViewState`, and never hold a request open across it.

**Settings action buttons are requests.** A schema `action` row ([authoring-settings.md §4](authoring-settings.md#4-notes-images-and-action-buttons)) fires `osfui.request("<yourmod>.<name>", { mod, key })` with a 5 s client timeout, so register it with `RegisterRequest`.

---

## 6. Native → web — state and events

The most consequential choice in this API, decided by one question: **is this value true until it changes, or did it happen?**

| | `SetViewState` | `SendToWeb` |
|---|---|---|
| Means | "this is what is true now" | "this just happened" |
| Arrives at | `osfui.state.on("<mod>/<key>")` | `osfui.on("<type>")` |
| On F5 / reload / crash recovery | replayed automatically | never replayed |
| Delivery | latest-wins, complete value per key | at most once |
| Audience | every live view of your mod, plus every one that loads later | one view id |

Wrong in one direction is the blank-after-F5 bug (state pushed as an event: correct until the player reloads, empty forever after). Wrong in the other re-fires a happening on every reload. Neither primitive can serve both, which is why there are two.

### 6a. SetViewState — true until it changes

```cpp
g_ui.SetViewState("acme.mymod", "ship", R"({"hull":88,"grav":3})");
```

Every live view of `acme.mymod` receives `{ kind:"state", mod, key, value }` at `osfui.state.on("acme.mymod/ship")` — **and so does every view of that mod that loads later**, because the runtime keeps the current value and replays it when each document greets the bridge. In 1.x, native state didn't exist: a plugin had to listen for some view-defined "I'm back" message and re-push, and whoever forgot shipped a HUD that was correct until the first F5.

- **Latest-wins per key, and the value is COMPLETE — never a delta.** A replay and a live update are the same message, so they carry the same thing.
- Any JSON *value* is legal, not just an object.
- Keys match case-insensitively and cap at 128 characters. At most **64 retained keys per mod**; a 65th distinct key is still delivered to live views but not retained (and logged), so it would survive until the next reload and then vanish. The cap exists to stop a mod generating keys in a loop.
- **Not session-scoped:** your state survives a save load. (Papyrus `SetView*` state is dropped on load, because its values can hold form identities. A plugin's HUD config has no such lifetime, and wiping it every load would be the bug.)
- Returns false **synchronously** on a null or malformed mod id, an empty or over-long key, unparseable JSON, or a saturated queue. The store write lands on the next main tick.
- Publishing with no view of yours open is not a lost write: the value is retained and replayed to your mod's first document.

With the JSON facade, no manual `dump()` and no temporary to keep alive:

```cpp
OSFUI::API::JsonClient jsonUi{ g_ui };
if (!jsonUi.SetViewState("acme.mymod", "ship", { { "hull", 88 }, { "grav", 3 } })) {
    // Synchronous refusal only: bad mod id, empty/over-long key, saturated queue.
    // (The JsonClient overloads are [[nodiscard]] — don't drop the result.)
}
```

`JsonClient` accepts JSON (or any `nlohmann`-convertible struct) for `SetViewState`, `SendToWeb`, `RegisterSettingsSchema`, `ReportIssue` and `ClearIssuesExcept` — every JSON-bearing call. The dependency-free `const char*` forms stay available.

### 6b. SendToWeb — something happened

```cpp
g_ui.SendToWeb("acme.mymod/dashboard", "acme.mymod.jumpComplete",
               R"({"system":"Alpha Centauri"})");
```

Delivers `{ kind:"event", name: <type>, payload: <payloadJson> }` to one view, arriving at its `osfui.on("acme.mymod.jumpComplete")`. `payloadJson` must be valid JSON; the call returns false only on null args or an unparseable payload.

An event is one-shot, delivered at most once, never replayed. A view that wasn't open when it fired never learns about it. Good events belong to a moment: a jump completed, a scan finished, the player was hit.

**Don't use an event to seed a view you're about to open.** Messages for a target that isn't live are held in bounded per-view queues, but a document that greets the bridge is treated as a *new* document and its queue is discarded — replaying a happening into a fresh page would re-fire its effect. So an event emitted before the target document greets is not delivered. State is the guaranteed path for anything the page should have at first paint, and the right shape for that data anyway.

Queues are bounded (drop-oldest, logged), so a view that never opens can't leak memory. A message addressed to a view discovery never found is dropped once the catalog is known.

### 6c. Opening a view

`RequestMenu(view, open)` opens or closes a surface by qualified `"<modId>/<viewName>"` id.

```cpp
g_ui.RegisterView("acme.mymod/dashboard");                       // validate; page stays lazy
g_ui.SetViewState("acme.mymod", "ship", R"({"hull":88})");       // seed it
g_ui.RequestMenu("acme.mymod/dashboard", true);                  // open
```

Issue all three back-to-back from any thread — they apply in order on the same tick, and the state reaches the page through its boot replay however much later it finishes loading.

Opening a discovered folder loads it on demand. Returns true if an open target exists and the request was queued; false if no such view was found. Closing works only on an already-loaded view and never loads one. True doesn't promise the page renders.

---

## 7. Status & readiness

All callable from any thread, synchronous:

- `GetInterfaceVersion()` — packed `(major << 16) | minor`.
- `GetPluginVersion(major, minor, patch)` — OSF UI's release.
- `GetBridgeProtocolVersion()` — the web protocol string. Informational.
- `IsBridgeReady()` — true when a bridge-enabled view is live.

To run code the moment the bridge goes live (and again after re-creation):

```cpp
g_ui.SetReadyCallback([](void*) noexcept {
    // fires on the main thread
}, nullptr);
```

You rarely need this. `SetViewState` before the bridge is ready is retained, not dropped, and `SendToWeb` is queued for a known target.

---

## 8. Settings, hotkeys, views, and health

Callable from any thread; callbacks fire on the game main thread. The JS side lives in [authoring-settings.md](authoring-settings.md).

### 8a. Settings

Typed getters — synchronous, any thread. They read a value mirror, not the store. Return false/0 on unknown key or wrong type.

```cpp
bool enabled = false;
g_ui.GetSettingBool("acme.mymod", "enabled", &enabled);

std::int64_t count = 0;
g_ui.GetSettingInt("acme.mymod", "count", &count);

double scale = 0.0;
g_ui.GetSettingFloat("acme.mymod", "scale", &scale);
```

`GetSettingString` handles string, enum (the option) and key (the key name, e.g. `"F10"` — a layout-independent physical position; see docs/authoring-settings.md §7). It returns the length including the NUL and always NUL-terminates; pass a null buffer to ask how big:

```cpp
std::uint32_t need = g_ui.GetSettingString("acme.mymod", "mode", nullptr, 0);
std::string buf(need ? need - 1 : 0, '\0');
if (need) g_ui.GetSettingString("acme.mymod", "mode", buf.data(), need);
```

(`type:"flags"` is a JSON array — no typed getter; read it from the change callback.)

Subscribe to a mod's values. Per-mod, not per-key — switch on the key. Current values are replayed once on subscribe, so no separate initial read.

```cpp
void OnSetting(const char* modId, const char* key,
               const char* valueJson,   // "true", "1.5", "\"compact\""
               void* user) noexcept
{
    // Main thread. Strings valid for this call only.
    // Be idempotent — the same value can arrive twice around the subscribe.
}

std::uint32_t token = g_ui.SubscribeSettings("acme.mymod", &OnSetting, nullptr);
g_ui.UnsubscribeSettings(token);   // 0 means the subscribe was rejected
```

Register a schema at runtime instead of shipping a file (same JSON):

```cpp
if (!g_ui.RegisterSettingsSchema(schemaJson)) {
    // false = bad JSON, non-object, or missing/invalid "id"
}
g_ui.UnregisterSettingsSchema("acme.mymod");   // keeps the user's saved values
```

User values overlay from the same file as the drop-in tier, so you can migrate from a file to a runtime registration without losing settings.

### 8b. Hotkeys

Fires when the key currently bound to a key-typed setting is pressed. You subscribe to the setting, not a key code — OSF UI re-resolves it on every rebind.

```cpp
void OnHotkey(const char* modId, const char* key, void* user) noexcept
{
    // Main thread. A hotkey is an event, not state — no replay.
}

std::uint32_t token = g_ui.SubscribeHotkey("acme.mymod", "openKey", &OnHotkey, nullptr);
g_ui.UnsubscribeHotkey(token);
```

Doesn't fire while the overlay captures text or during a rebind, and key repeats don't fire. Conflicting bindings across mods all fire — the settings UI flags conflicts but never blocks them.

### 8c. Views

`RegisterView` validates a discovered `views/<modId>/<viewName>/` folder your mod ships. The view appears in the Mods surface and responds to `RequestMenu` and the web `menu.open`; its WebView2 page is created only when first opened unless its manifest has `openOnStart:true`.

- Idempotent — an already-live view isn't reloaded.
- A missing folder just warns (ship the folder with your mod).
- `openOnStart` from the manifest creates and opens the view immediately, menus included. An explicit `RegisterView` is plugin opt-in, unlike discovery, where menus never auto-start and a HUD's `openOnStart` is only the default under the player's per-HUD auto-start choice.
- Returns false only on a null/invalid id.

It's an optional declaration for a **plugin-shipped** folder. A plain drop-in view is found at boot and loads on first open with no plugin at all.

### 8d. Session health

Report a condition into OSF UI's **System Health** pane — the one place a player looks when something is wrong, whichever mod noticed it. Don't build a second health page in your own view.

```cpp
constexpr const char* kMod = "acme.mymod";

// Something is wrong, and stays wrong until it isn't.
g_ui.ReportIssue(kMod, "pack-parse:highlights", "catalog.parse-failed",
                 OSFUI::API::IssueSeverity::kError, "highlights",
                 R"({"file":"highlights.json","line":12})");

// It cleared.
g_ui.ClearIssue(kMod, "pack-parse:highlights");
```

**This is not a log channel and not a toast.** Report only what is *durable* (still true when the player reads it), *actionable*, and *worth interrupting them for*. Routine progress, one-frame hiccups and anything already self-corrected belong in your log.

Identity, not events:

- `id` is **your** dedupe key. Re-reporting a live id bumps its occurrence count in place — that's what distinguishes "once at startup" from "every few seconds" — instead of stacking cards.
- `code` is **your** stable machine code for the *kind* of condition. Never prose: OSF UI owns the wording so it stays localizable and no mod writes the words on its own card. An unknown code renders as a card naming your mod with your context as technical detail — degraded, never broken.
- `ClearIssue` moves it to **Resolved this session**, which is what a player wants after a retry. Cheap to call unconditionally.
- Recomputing a whole set? Report what's wrong now, then sweep: `ClearIssuesExcept(kMod, R"(["still-bad-1","still-bad-2"])")`. FIFO with `ReportIssue`, so the pair lands correctly in one tick. **The sweep is scoped to your mod, not to one producer inside it** — if your plugin reports from several places, the keep list must name every id you still want live, or the rest are withdrawn as collateral.

Everything is namespaced to the calling mod: the issue's `source` is **your mod id, assigned by the host** — never a parameter — and your ids and codes are prefixed with it. Two mods can use the same local id without colliding, and no mod can resolve or overwrite a platform issue.

`context` is optional bounded detail: a **flat JSON object** of string/number/bool values, capped at 8 entries and 240 chars per value. It's sanitized on the way in — anything path-, URL- or command-shaped is cut to its trailing component, because an absolute path identifies the player's machine and account. Pass bare filenames and ids; don't rely on nested values surviving.

---

## 9. Threading & lifetime

**Threading**

- Any thread, synchronous: all status reads, the typed setting getters, and the validation half of every mutating call.
- Any thread, applied next tick: the *effect* of every mutating call (register, send, publish state, subscribe, request menu, report issue).
- Always the main thread: every callback (`CommandFn`, `RequestFn`, `ReadyFn`, `SettingChangedFn`, `HotkeyFn`). Keep them cheap.
- `Request::Respond` / `Reject` are the exception: safe from any thread, at any later time.

**Lifetime**

- `const char*` args passed **into** callbacks are valid only during the call. Copy anything you keep.
- Callbacks can fire for the whole process — registrations survive bridge re-creation. Don't point one at something you might free; use static/leaked state, or unregister first.
- Settings replay can deliver the same value twice. Make `SettingChangedFn` idempotent.
- Strings returned by the API are static, valid for the process.
- Retained view state outlives every document and every save load; it's dropped only at runtime shutdown. Don't put a session-scoped identity in it.
- OSF UI owns the bridge; never delete it.

---

## 10. Method reference

All on `IOSFUIBridge`, mirrored on `Client` (which adds the version gate). **Since** is the additive ABI minor used by the `Client` feature gates.

| Method | Since | Thread | Notes |
|---|---|---|---|
| `GetInterfaceVersion()` | 1.0 | any | packed `(major<<16)｜minor` |
| `GetPluginVersion(maj,min,pat)` | 1.0 | any | OSF UI release |
| `GetBridgeProtocolVersion()` | 1.0 | any | don't parse |
| `IsBridgeReady()` | 1.0 | any | a view is live |
| `RegisterCommand(cmd,fn,user)` | 1.0 | any | send + compatibility auto-ack request; shape `<author>.<modname>.<name>` |
| `UnregisterCommand(cmd)` | 1.0 | any | |
| `RegisterRequest(name,fn,user)` | 1.7 | any | first-wins across commands and requests; callback on main |
| `UnregisterRequest(name)` | 1.7 | any | in-flight tokens stay valid until answer/timeout/close |
| `SendToWeb(view,type,json)` | 1.0 | any | one-shot EVENT to one view; queued, never replayed |
| `SetViewState(mod,key,json)` | 1.8 | any | retained STATE; replayed to every fresh document |
| `SetReadyCallback(fn,user)` | 1.0 | any | fires on main thread |
| `RequestMenu(view,open)` | 1.1 | any | open loads on demand; close needs a loaded view |
| `SubscribeSettings(mod,fn,user)` | 1.2 | any | replayed on subscribe; returns token/0 |
| `UnsubscribeSettings(token)` | 1.2 | any | |
| `GetSettingBool/Int/Float(mod,key,out)` | 1.2 | any | false/0 on miss |
| `GetSettingString(mod,key,buf,len)` | 1.2 | any | returns length incl. NUL; null buf = probe |
| `RegisterSettingsSchema(json)` | 1.2 | any | false on bad JSON/shape |
| `UnregisterSettingsSchema(mod)` | 1.2 | any | keeps saved values |
| `SubscribeHotkey(mod,key,fn,user)` | 1.4 | any | no replay; returns token/0 |
| `UnsubscribeHotkey(token)` | 1.4 | any | |
| `RegisterView(view)` | 1.5 | any | `<modId>/<viewName>`; idempotent |
| `ReportIssue(mod,id,code,sev,subj,ctx)` | 1.7 | any | false on bad mod id / empty id or code / non-object context |
| `ClearIssue(mod,id)` | 1.7 | any | true = queued, not "was active" |
| `ClearIssuesExcept(mod,keepJson)` | 1.7 | any | keep list is a JSON array of ids |

---

## 11. Example plugin

Surfaces its own view, publishes state it can never be asked to re-send, answers one request, takes one command, reacts to a setting, and never hard-fails when OSF UI is absent.

```cpp
#include "OSFUI_API.h"
#include "OSFUI_JSON.h"
using namespace OSFUI::API;

static Client     g_ui;
static JsonClient g_json{ g_ui };

// STATE: true until it changes, so the runtime replays it to every document.
static void PublishShip()
{
    if (!g_json.SetViewState("acme.mymod", "ship", {
            { "hull", CurrentHull() },
            { "grav", CurrentGrav() },
        })) {
        LogRefusedStateWrite();   // a shape bug in this call, never a delivery failure
    }
}

// A REQUEST: the view wants an answer.
static void OnGetWeight(const Request& raw, void*) noexcept
{
    JsonRequest request{ raw };
    if (!request) return;
    const auto formId = request.Get<std::uint32_t>("formId");
    if (!formId) return;
    request.Respond({ { "weight", ReadWeight(*formId) } });
}

// A COMMAND: one-way. Nothing is sent back, and nothing needs to be.
static void OnDock(const char*, const char*, const char*, void*) noexcept
{
    BeginDocking();
}

static void OnSetting(const char*, const char* key, const char* valueJson, void*) noexcept
{
    if (std::strcmp(key, "enabled") == 0) {
        // valueJson is "true" / "false"
    }
}

// Call once from SFSE kPostLoad.
void InitOsfUi()
{
    if (!g_ui.Init()) return;   // OSF UI absent or 1.x-era — degrade silently

    g_ui.RegisterRequest("acme.mymod.getWeight", &OnGetWeight, nullptr);
    g_ui.RegisterCommand("acme.mymod.dock", &OnDock, nullptr);
    g_ui.SubscribeSettings("acme.mymod", &OnSetting, nullptr);

    g_ui.RegisterView("acme.mymod/dashboard");
    PublishShip();                                     // seeds the page whenever it loads
    g_ui.RequestMenu("acme.mymod/dashboard", true);
}
```

The view's half is three lines, with no lifecycle code:

```js
osfui.state.on("acme.mymod/ship", (ship) => render(ship));
document.querySelector("#dock").onclick = () => osfui.send("acme.mymod.dock");
const { weight } = await osfui.request("acme.mymod.getWeight", { formId });
```

---

## 12. See also

- [`sdk/OSFUI_API.h`](../sdk/OSFUI_API.h) — the header (source of truth).
- [`sdk/README.md`](../sdk/README.md) — SDK overview.
- [mod-api-2.0-design.md](mod-api-2.0-design.md) — why the four verbs, and what 2.0 deleted.
- [authoring-views.md](authoring-views.md) — the view (JS) side, `window.osfui`.
- [authoring-settings.md](authoring-settings.md) — settings schemas and reading them.
- [authoring-dynamic-data.md](authoring-dynamic-data.md) — the Papyrus half of the same grid.
- [security-model.md](security-model.md) — where native endpoint registration sits in the trust model.
