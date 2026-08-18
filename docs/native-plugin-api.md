# Native plugin API

Lets your DLL talk to OSF UI: handle messages a view sends, publish state and events back, read settings and hotkeys, and open views.

The stable, dependency-free ABI is [`sdk/OSFUI_API.h`](../sdk/OSFUI_API.h) (C ABI **2.0**). If your plugin already uses `nlohmann::json`, include the optional [`sdk/OSFUI_JSON.h`](../sdk/OSFUI_JSON.h) authoring facade too — it stays on your side of the DLL boundary and never changes the ABI.

Component, version, endpoint, and lifecycle names follow the
[terminology glossary](terminology.md).

Writing a view (HTML/JS) instead? [authoring-views.md](authoring-views.md) is the `window.osfui` side. Your `SetViewState` arrives at the view's `osfui.state.on`, your `SendToWeb` at its `osfui.on`, and its `osfui.send` / `osfui.request` arrive at your handlers.

**Contents**

- [0. When you need this](#0-when-you-need-this)
- [1. Additive ABI 1.x](#1-additive-abi-1x)
- [2. Get the bridge](#2-get-the-bridge)
- [3. Versioning](#3-versioning)
- [4. Sends — web → native](#4-sends--web--native)
- [5. Requests — web → native, settled once](#5-requests--web--native-settled-once)
- [6. Native → web — state and events](#6-native--web--state-and-events)
- [7. Status & readiness](#7-status--readiness)
- [8. Settings, hotkeys, and views](#8-settings-hotkeys-and-views)
- [8d. System Health publication](#8d-system-health-publication)
- [9. Threading & lifetime](#9-threading--lifetime)
- [10. Method reference](#10-method-reference)
- [11. Example plugin](#11-example-plugin)
- [12. See also](#12-see-also)

---

## 0. When you need this

Most mods need no native code:

- Ship a view: drop a folder in `views/<modId>/<viewName>/` ([authoring-views.md](authoring-views.md)).
- Ship settings: drop a schema in `settings/<modId>.json` ([authoring-settings.md](authoring-settings.md)).
- A view reads/writes its own settings and reacts to hotkeys from JS.
- Papyrus can publish view state, send view events and answer view requests with no DLL ([authoring-dynamic-data.md](authoring-dynamic-data.md)).

Use this API when your logic is in a native DLL and needs to handle view messages (and answer the ones needing an answer), publish game state or push a one-shot happening, read settings or react to changes from C++, react to a hotkey, register a schema or view folder dynamically, open or close a view, or publish a durable local condition into System Health.

If OSF UI is missing, every call is a safe no-op — you never special-case "not installed". Existing ABI 1.x binaries remain supported; rebuild when you want a newer tail feature.

---

## 1. Additive ABI 1.x

`kBridgeAPIVersion` is `(1 << 16) | 9`. The ABI is append-only: all published slots retain their order and behavior, and a new capability is appended at the vtable tail with a higher minor. ABI 1.8 appended retained `SetViewState`; ABI 1.9 appends strict `RegisterSend` / `UnregisterSend`. A send receives exactly the caller's payload and never produces an acknowledgement; a request naming it is rejected `wrong-endpoint-kind`.

Older 1.x binaries receive the same bridge and use only the prefix they were compiled against. The frozen `RegisterCommand` slots keep their request-ID injection and automatic-reply behavior; use `RegisterSend` for a strict one-way endpoint and `RegisterRequest` for an explicit result. A different ABI major receives `nullptr` plus an unsupported-ABI error in System Health.

Future additions append methods at the vtable tail and bump the minor. The `Client` wrapper feature-gates those additions.

---

## 2. Get the bridge

`OSFUI.dll` exports one C function, `OSFUI_RequestBridge`. Don't call it directly — use `OSFUI::API::Client`, which fetches the bridge, caches the running OSF UI release version once, and turns any call that release is too old for into a safe no-op.

```cpp
#include "OSFUI_API.h"

static OSFUI::API::Client g_ui;

// Call ONCE, after SFSE kPostLoad.
void OnPostLoad()
{
    if (g_ui.Init()) {            // false if OSF UI is absent or a major apart
        g_ui.RegisterSend("acme.mymod.equip", &OnEquip, nullptr);
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
| **C ABI** (`2.0`) | `GetInterfaceVersion()` | which native methods exist. Packed `(major<<16)｜minor`. |
| **Plugin** (OSF UI release) | `GetPluginVersion()` | nothing — log it for support. |
| **Web protocol** (`"2.0"`) | `GetBridgeProtocolVersion()` | the JS handshake. Native code: don't parse it. |

`Feature` values are their additive ABI minors. Every feature in the 2.0 header is baseline; `Client::Has(Feature)` remains the gate for future 2.x tail additions. Major mismatches are refused.

---

## 4. Sends — web → native

A view calls `osfui.send("<name>", payload)`. Your handler runs on the game main thread.

```cpp
void OnEquip(const char* name,
             const char* payloadJson,   // the caller's payload object, verbatim
             const char* sourceViewId,  // who sent it
             void* user) noexcept
{
    // All const char* args are valid only during this call. Copy what you keep.
}

g_ui.RegisterSend("acme.mymod.equip", &OnEquip, nullptr);
```

A registered send is strictly one-way. A `request()` naming it is rejected `wrong-endpoint-kind`; no `requestId` or other routing field is injected into the payload and no acknowledgement is generated. Use `RegisterRequest` whenever the view needs a result or failure. A `send()` naming a request endpoint is dropped and surfaced as `wrong-endpoint-kind`.

**Names:** endpoint names are opaque non-empty strings. `<modId>.<name>` remains the recommended readable convention, but dots are not parsed and there is no minimum count. OSF UI explicitly refuses every current platform endpoint plus the case-insensitive `osfui` / `osfui.*` namespace.

**Duplicates are first-wins across both registries.** A name can't be both a send and a request, and registering a name someone else owns is refused. To replace your own handler, `UnregisterSend` then register again — the pair works back-to-back within one tick.

Register once at `kPostLoad`.

---

## 5. Requests — web → native, settled once

Use a request when the view needs an answer. It settles exactly once: a payload,
a typed error, or the OSF UI runtime's deadline.

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

For a fire-and-forget `RegisterSend` callback, `JsonSend` gives the same typed payload access plus `Name()` and `SourceViewId()`:

```cpp
void OnEquip(const char* name, const char* json, const char* source, void*) noexcept
{
    OSFUI::API::JsonSend event{ name, json, source };
    if (!event) return;
    std::uint32_t formId{};
    if (event.TryGet("formId", formId)) Equip(formId);
}
```

**Settling.** The OSF UI runtime owns correlation: `RequestFn` receives no
`requestId`. Copy the `Request` value if you answer later; `Respond` and
`Reject` are safe from any thread and settle once — a second answer is ignored
and logged. The compatibility field `command` contains the request endpoint
name; its pointer, `payloadJson`, and `sourceViewId` are valid only during the
callback.

The token is a copyable C-ABI value holding an opaque 64-bit id and OSF UI
runtime function pointers, not an OSF UI runtime-owned object pointer. A saved copy
can't dereference freed OSF UI runtime memory: after response, timeout or view closure
its id is stale and a late answer is a logged no-op.

**Failure.** `req.Reject("stable-code", "human detail")` — the view's `osfui.request()` rejects with an error carrying that `code`, and the shared bridge helper prints it to that page's console with an `[osfui]` prefix, so your rejection reason lands in F12 DevTools with the payload attached, not only in `OSF UI.log`. Invalid response JSON rejects as `invalid-response`.

**Limits.** Requests use the same qualified grammar and first-wins namespace as sends. Each view may hold at most 64 requests in flight; overflow rejects with `request-capacity`. The fixed OSF UI runtime deadline is 30 seconds, no opt-out, after which the view is answered `no-response`. For genuinely long work, take a *send*, publish progress and results with `SetViewState`, and never hold a request open across it.

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
| Audience | every current and future document instance of your mod | one qualified view id |

Wrong in one direction is the blank-after-F5 bug (state pushed as an event: correct until the player reloads, empty forever after). Wrong in the other re-fires a happening on every reload. Neither primitive can serve both, which is why there are two.

### 6a. SetViewState — true until it changes

```cpp
g_ui.SetViewState("acme.mymod", "ship", R"({"hull":88,"grav":3})");
```

Every current document instance of `acme.mymod` receives `{ kind:"state", mod, key, value }` at `osfui.state.on("acme.mymod/ship")` — **and so does every future document instance of that mod**, because the OSF UI runtime keeps the current value and replays it when each document greets the bridge. In 1.x, native state didn't exist: a plugin had to listen for some view-defined "I'm back" message and re-push, and whoever forgot shipped a HUD that was correct until the first F5.

- **Latest-wins per key, and the value is COMPLETE — never a delta.** A replay and a live update are the same message, so they carry the same thing.
- Any JSON *value* is legal, not just an object.
- Keys match case-insensitively and cap at 128 characters. At most **64 retained keys per mod**; a 65th distinct key is still delivered to current document instances but not retained (and logged), so it would survive until the next reload and then vanish. The cap exists to stop a mod generating keys in a loop.
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

`JsonClient` accepts JSON (or any `nlohmann`-convertible struct) for `SetViewState`, `SendToWeb`, `RegisterSettingsSchema`, `ReportIssue`, and `ClearIssuesExcept`. The dependency-free `const char*` forms stay available.

### 6b. SendToWeb — something happened

```cpp
g_ui.SendToWeb("acme.mymod/dashboard", "acme.mymod.jumpComplete",
               R"({"system":"Alpha Centauri"})");
```

Delivers `{ kind:"event", name: <type>, payload: <payloadJson> }` to one view, arriving at its `osfui.on("acme.mymod.jumpComplete")`. `payloadJson` must be valid JSON; the call returns false only on null args or an unparseable payload.

An event is one-shot, delivered at most once, never replayed to a later document instance. Good events belong to a moment: a jump completed, a scan finished, the player was hit.

`SendToWeb` has a bounded holdback for a known view that is not yet
instantiated. When that view is instantiated, its fresh event gate queues the
held messages until the document's first greeting, then flushes them after
`ready` and state. This preserves the documented
`RegisterView` → `SendToWeb` → `RequestMenu` ordering. Creating a replacement
view clears any gate left by the prior document, and a delivered event is not
retained for a later reload.

**Don't use an event to seed a view you're about to open.** The holdback is
bounded and exists for ordering, not persistence. State is the guaranteed path
for anything the page should have at first paint and after every later document
recreation, and it is the right semantic shape for that data anyway.

Queues are bounded (drop-oldest, logged), so a view that never opens can't leak memory. A message addressed to a view that discovery never found is dropped once the catalog is known.

### 6c. Opening a view

`RequestMenu(view, open)` opens or closes a view by qualified
`"<modId>/<viewName>"` id. The API name is retained for compatibility and also
accepts HUD views.

```cpp
g_ui.RegisterView("acme.mymod/dashboard");                       // validate; view stays uninstantiated
g_ui.SetViewState("acme.mymod", "ship", R"({"hull":88})");       // seed it
g_ui.RequestMenu("acme.mymod/dashboard", true);                  // open
```

Issue all three back-to-back from any thread — they apply in order on the same tick, and the state reaches the page through its boot replay however much later it finishes loading.

Opening a discovered view instantiates it on demand. Returns true if an open target exists and the request was queued; false if no such view was found. Closing works only on an already-instantiated view and never instantiates one. True doesn't promise the document renders.

---

## 7. Status & readiness

All callable from any thread, synchronous:

- `GetInterfaceVersion()` — packed `(major << 16) | minor`.
- `GetPluginVersion(major, minor, patch)` — OSF UI's release.
- `GetBridgeProtocolVersion()` — the web bridge protocol version string. Informational.
- `IsBridgeReady()` — compatibility name for bridge availability; true when at least one bridge-enabled view is instantiated.

To run code when bridge availability first becomes true (and again after
browser-host recovery makes it available):

```cpp
g_ui.SetReadyCallback([](void*) noexcept {
    // fires on the main thread
}, nullptr);
```

You rarely need this. `SetViewState` before the bridge is available is retained, not dropped, and `SendToWeb` is queued for a known target.

---

## 8. Settings, hotkeys, and views

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

Register a schema dynamically instead of shipping a file (same JSON):

```cpp
if (!g_ui.RegisterSettingsSchema(schemaJson)) {
    // false = bad JSON, non-object, or missing/invalid "id"
}
g_ui.UnregisterSettingsSchema("acme.mymod");   // keeps the user's saved values
```

User values overlay from the same file as the drop-in tier, so you can migrate from a file to native-plugin registration without losing settings.

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

`RegisterView` validates a discovered `views/<modId>/<viewName>/` folder your mod ships. The path supplies the qualified view id; `manifest.json` declares no identity field. A catalog-visible view appears in Mod Settings and responds to `RequestMenu` and the web `menu.open`; the view is instantiated only when first opened unless its manifest has `openOnStart:true`, which explicitly opens it on registration.

- Idempotent — an already-instantiated view isn't reloaded.
- A missing folder just warns (ship the folder with your mod).
- `openOnStart` creates and opens the explicitly registered view immediately,
  menus included. This is **open on registration** and is plugin opt-in. It is
  distinct from normal discovery, where menus never auto-start and a HUD's
  `openOnStart` is only the author's auto-start default under the player's
  per-HUD choice.
- Returns false only on a null/invalid id.

It's an optional declaration for a **plugin-shipped** folder. A plain drop-in view is found at boot and loads on first open with no plugin at all.

### 8d. System Health publication

`ReportIssue` publishes a durable, actionable condition to the built-in System Health destination. This is local session state—not a log channel, report submission, upload, toast, or external issue opener.

```cpp
constexpr const char* kMod = "acme.mymod";

g_ui.ReportIssue(kMod, "pack-parse:highlights", "catalog.parse-failed",
    OSFUI::API::IssueSeverity::kError, "highlights",
    R"({"file":"highlights.json","line":12})");

g_ui.ClearIssue(kMod, "pack-parse:highlights");
```

- `id` is the producer-local dedupe key. Re-reporting an active id increments its occurrence count instead of adding a card.
- `code` is stable machine identity, not player-facing prose. Unknown codes still render with bounded technical context.
- `ClearIssue` moves the condition to resolved session history.
- `ClearIssuesExcept(mod, keepIdsJson)` reconciles a recomputed set. The keep list is scoped to the whole mod and ordered FIFO with preceding reports.
- Context must be a JSON object. The registry bounds and sanitizes values, strips path/URL/command-shaped data to its trailing component, and never uploads or opens anything from it.

## 9. Threading & lifetime

**Threading**

- Any thread, synchronous: all status reads, the typed setting getters, and the validation half of every mutating call.
- Any thread, applied next tick: the *effect* of every mutating call (register, send, publish state, subscribe, request menu).
- Always the main thread: every callback (`SendFn`, `RequestFn`, `ReadyFn`, `SettingChangedFn`, `HotkeyFn`). Keep them cheap.
- `Request::Respond` / `Reject` are the exception: safe from any thread, at any later time.

**Lifetime**

- `const char*` args passed **into** callbacks are valid only during the call. Copy anything you keep.
- Callbacks can fire for the whole process — registrations survive bridge re-creation. Don't point one at something you might free; use static/leaked state, or unregister first.
- Settings replay can deliver the same value twice. Make `SettingChangedFn` idempotent.
- Strings returned by the API are static, valid for the process.
- Retained mod state published through the compatibility-named `SetViewState`
  outlives every document instance and every save load; it is scoped to the
  publishing mod and dropped only at OSF UI runtime shutdown. Don't put a
  session-scoped identity in it.
- OSF UI owns the bridge; never delete it.

---

## 10. Method reference

All on `IOSFUIBridge`, mirrored on `Client`. The `Since` column is the ABI 1.x minor required for that slot; the wrapper returns a safe no-op when attached to an older host.

| Method | Since | Thread | Notes |
|---|---|---|---|
| `GetInterfaceVersion()` | 1.0 | any | packed `(major<<16)｜minor` |
| `GetPluginVersion(maj,min,pat)` | 1.0 | any | OSF UI release |
| `GetBridgeProtocolVersion()` | 1.0 | any | don't parse |
| `IsBridgeReady()` | 1.0 | any | a bridge-enabled view is instantiated |
| `RegisterCommand(name,fn,user)` | 1.0 | any | frozen send-or-auto-ack request behavior; prefer a strict endpoint below |
| `UnregisterCommand(name)` | 1.0 | any | |
| `RegisterSend(name,fn,user)` | 1.9 | any | strict one-way send; opaque name outside the reserved platform surface |
| `UnregisterSend(name)` | 1.9 | any | |
| `RegisterRequest(name,fn,user)` | 1.7 | any | first-wins across endpoint kinds; callback on main |
| `UnregisterRequest(name)` | 1.7 | any | in-flight tokens stay valid until answer/timeout/close |
| `SendToWeb(view,type,json)` | 1.0 | any | one-shot EVENT to one view; queued, never replayed |
| `SetViewState(mod,key,json)` | 1.8 | any | retained STATE; replayed to every fresh document |
| `SetReadyCallback(fn,user)` | 1.0 | any | fires on main thread |
| `RequestMenu(view,open)` | 1.1 | any | open instantiates on demand; close needs an instantiated view |
| `SubscribeSettings(mod,fn,user)` | 1.2 | any | replayed on subscribe; returns token/0 |
| `UnsubscribeSettings(token)` | 1.2 | any | |
| `GetSettingBool/Int/Float(mod,key,out)` | 1.2 | any | false/0 on miss |
| `GetSettingString(mod,key,buf,len)` | 1.2 | any | returns length incl. NUL; null buf = probe |
| `RegisterSettingsSchema(json)` | 1.2 | any | false on bad JSON/shape |
| `UnregisterSettingsSchema(mod)` | 1.2 | any | keeps saved values |
| `SubscribeHotkey(mod,key,fn,user)` | 1.4 | any | no replay; returns token/0 |
| `UnsubscribeHotkey(token)` | 1.4 | any | |
| `RegisterView(view)` | 1.5 | any | `<modId>/<viewName>`; idempotent |
| `ReportIssue(mod,id,code,sev,subject,context)` | 1.7 | any | publishes a local System Health condition; false on invalid identity or non-object context |
| `ClearIssue(mod,id)` | 1.7 | any | true means queued, not “was active” |
| `ClearIssuesExcept(mod,keepJson)` | 1.7 | any | keep list is a JSON array of ids |

---

## 11. Example plugin

Provides its own view, publishes state it can never be asked to re-send, answers one request, takes one send, reacts to a setting, and never hard-fails when OSF UI is absent.

```cpp
#include "OSFUI_API.h"
#include "OSFUI_JSON.h"
using namespace OSFUI::API;

static Client     g_ui;
static JsonClient g_json{ g_ui };

// STATE: true until it changes, so the OSF UI runtime replays it to every document.
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

// A SEND: one-way. Nothing is sent back, and nothing needs to be.
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
    if (!g_ui.Init()) return;   // OSF UI absent or ABI-mismatched — degrade silently

    g_ui.RegisterRequest("acme.mymod.getWeight", &OnGetWeight, nullptr);
    g_ui.RegisterSend("acme.mymod.dock", &OnDock, nullptr);
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
