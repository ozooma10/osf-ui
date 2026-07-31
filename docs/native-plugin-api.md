# Native plugin API

Lets your DLL communicate with OSF UI. 
Handle commands from a view, push data to a view, read settings and hotkeys, and open views.

The stable, dependency-free ABI is [`sdk/OSFUI_API.h`](../sdk/OSFUI_API.h) (C ABI **1.7**). If your plugin uses `nlohmann::json`, include the optional [`sdk/OSFUI_JSON.h`](../sdk/OSFUI_JSON.h) authoring facade as well; it remains entirely on your side of the DLL boundary.

Writing a view (HTML/JS) instead? See [authoring-views.md](authoring-views.md) - that's the `window.osfui` side. Your `SendToWeb` lands at a view's `osfui.onMessage`; a view's `osfui.send` lands at your command handler.

**Contents**

- [0. When you need this](#0-when-you-need-this)
- [1. Get the bridge](#1-get-the-bridge)
- [2. Versioning](#2-versioning)
- [3. Commands and requests (web → native)](#3-commands-web--native)
  - [3a. Request/response](#3a-requestresponse)
- [4. Status & readiness](#4-status--readiness)
- [5. Settings, hotkeys, views, and health](#5-settings-hotkeys-views-and-health)
  - [5a. Settings](#5a-settings)
  - [5b. Hotkeys](#5b-hotkeys)
  - [5c. Views](#5c-views)
  - [5d. Session health](#5d-session-health)
- [6. Native → web messaging](#6-native--web-messaging)
  - [6a. SendToWeb](#6a-sendtoweb)
- [7. Threading & lifetime](#7-threading--lifetime)
- [8. Method reference](#8-method-reference)
- [9. Example plugin](#9-example-plugin)
- [10. See also](#10-see-also)

---

## 0. When you need this

Most mods need no native code:

- Ship a view: drop a folder in `views/<modId>/<viewName>/`. See [authoring-views.md](authoring-views.md).
- Ship settings: drop a schema in `settings/<author>.<modname>.json`. See [authoring-settings.md](authoring-settings.md).
- A view reads/writes its own settings and reacts to hotkeys from JS.

Use this API when your logic is in a native DLL and needs to:

- handle commands a view sends,
- push game state into a view,
- read settings, or react to them changing, from C++,
- react to a hotkey from C++,
- register a schema or view folder at runtime,
- open or close a view.

If OSF UI is missing or a major version apart, calls no-op so you never have to special-case "OSF UI not installed."

---

## 1. Get the bridge

`OSFUI.dll` exports one C function, `OSFUI_RequestBridge`. Don't call it directly - use `OSFUI::API::Client`. It fetches the bridge, caches the host version once, and makes any call the host is too old for a safe no-op.

```cpp
#include "OSFUI_API.h"

static OSFUI::API::Client g_ui;

// Call ONCE, after SFSE kPostLoad.
void OnPostLoad()
{
    if (g_ui.Init()) {            // false if OSF UI is absent or a major apart
        g_ui.RegisterCommand("acme.mymod.ping", &OnPing, nullptr);
        g_ui.RegisterView("acme.mymod/dashboard");
    }
}
```

`Init()` returns false when OSF UI isn't installed, isn't loaded yet, or its major differs from your header. 

Fetch after `kPostLoad`, once. The bridge lives for the whole process; the `static Client` above caches it. Don't resolve the export per-frame.

`Client::Raw()` gives you the raw `IOSFUIBridge*` for advanced use

---

## 2. Versioning

Three separate version numbers.

| Version | Read it with | Gates |
|---|---|---|
| **C ABI** (`1.7`) | `GetInterfaceVersion()` | which native methods exist. Gate on this. |
| **Plugin** (OSF UI release) | `GetPluginVersion()` | nothing - log it for support. |
| **Web protocol** (e.g. `"1.0"`) | `GetBridgeProtocolVersion()` | the JS handshake. Native code: don't parse it. |


---

## 3. Commands (web -> native)

A view calls `osfui.send("<command>", payload)`. Your handler runs on the game main thread.

```cpp
void OnPing(const char* command,
            const char* payloadJson,   // e.g. "{\"id\":\"x\",\"requestId\":\"7\"}"
            const char* sourceViewId,  // who sent it - your reply target
            void* user) noexcept
{
    // All const char* args are valid only during this call. Copy what you keep.
    g_ui.SendToWeb(sourceViewId, "acme.mymod.pong", "{\"ok\":true}");
}

g_ui.RegisterCommand("acme.mymod.ping", &OnPing, nullptr);
```

**Command names (ABI 1.6):** must be `<author>.<modname>.<name>` - a mod id
(lowercase `[a-z0-9-]`, exactly one dot) plus a name that may contain more dots.
So two dots minimum. Bad shapes are refused with a log warning. Platform
commands are dotless or single-dot, so you can't collide with them.

**Duplicates are first-wins:** registering a name someone else owns is refused.
To replace your own handler, `UnregisterCommand` then register again.

**Replies:** the payload may carry a `requestId`. When your handler returns, the
host acks the caller with `ui.result { ok:true }` (delivered, not succeeded).
There's no return value — send real results back with `SendToWeb`, echoing the
`requestId` if you want the view to correlate them.

Register once at `kPostLoad`.

### 3a. Request/response

ABI 1.7 (`Feature::kRequests`) adds a first-class value-returning path. Use it
when the view needs an answer; keep `RegisterCommand` for fire-and-forget work.

The preferred authoring form uses the optional JSON facade:

```cpp
#include "OSFUI_API.h"
#include "OSFUI_JSON.h"

static void OnGetWeight(const OSFUI::API::Request& raw, void*) noexcept
{
    OSFUI::API::JsonRequest request{ raw };
    if (!request) return; // malformed input was already rejected

    const auto formId = request.Get<std::uint32_t>("formId");
    if (!formId) return; // missing/wrong type was rejected as invalid-payload
    request.Respond("acme.mymod.weight", {
        { "weight", ReadWeight(*formId) }
    });
}

g_ui.RegisterRequest("acme.mymod.getWeight", &OnGetWeight, nullptr);
```

```js
const { weight } = await osfui.call("acme.mymod.getWeight", { formId });
```

`JsonRequest::Get<T>(key)` reads a required field without throwing and rejects
a missing or wrongly typed field as `invalid-payload`. `Value<T>(key,
fallback)` and `TryGet()` handle optional fields. `Require<T>()` and `As<T>()`
retain normal `nlohmann::json` exceptions for code that deliberately wants
them. Malformed or non-object payloads are also rejected as
`invalid-payload`; response serialization failures reject as
`serialization-error`. Use `Raw()` when an advanced path needs the copyable
deferred token directly.

For a fire-and-forget `RegisterCommand` callback, `JsonCommand` provides the
same typed payload access plus `Command()` and `SourceViewId()`:

```cpp
void OnEquip(const char* command, const char* json, const char* source, void*) noexcept
{
    OSFUI::API::JsonCommand event{ command, json, source };
    if (!event) return;
    std::uint32_t formId{};
    if (event.TryGet("formId", formId)) Equip(formId);
}
```

The host owns correlation: `RequestFn` receives no `requestId`. Copy the
`Request` value if you answer later; `Respond` and `Reject` are safe from any
thread and settle once. A second answer is ignored and logged. The `command`,
`payloadJson`, and `sourceViewId` pointers are valid only during the callback.

The token is a copyable C-ABI value containing an opaque 64-bit id and host
function pointers, rather than a host-owned object pointer. A saved copy cannot
dereference freed host memory: after response, timeout, or view closure its id
is stale and a late answer is a logged no-op.

Requests use the same qualified grammar and first-wins namespace as commands.
A name cannot be both. Each view may hold at most 64 requests in flight;
overflow rejects immediately with `request-capacity`. The fixed host timeout is
30 seconds with no opt-out, after which the view receives
`ui.error { code:"no-response" }`. For genuinely long work, acknowledge a
command and push progress/results as events instead of retaining a request.

Report a plugin failure with `req.Reject("stable-code", "human detail")`. It
becomes correlated `ui.error`, so `osfui.request()` rejects with that
`error.code`. Invalid response JSON rejects as `invalid-response`.
---

## 4. Status & readiness

All callable from any thread, synchronous:

- `GetInterfaceVersion()` - packed `(major << 16) | minor`.
- `GetPluginVersion(major, minor, patch)` - OSF UI's release.
- `GetBridgeProtocolVersion()` - the web protocol string. Informational.
- `IsBridgeReady()` - true when a bridge-enabled view is live.

To run code the moment the bridge goes live (and again after re-creation):

```cpp
g_ui.SetReadyCallback([](void*) noexcept {
    // fires on the main thread
}, nullptr);
```

You rarely need this. You can `SendToWeb` before the bridge is ready and the message is queued, not dropped.

---

## 5. Settings, hotkeys, views, and health

Callable from any thread; callbacks fire on the game main thread. 
The JS side of these lives in [authoring-settings.md](authoring-settings.md).

### 5a. Settings

ABI 1.2 (`Feature::kSettings`).

Typed getters — synchronous, any thread. They read a value mirror, not the
store. Return false/0 on unknown key or wrong type.

```cpp
bool enabled = false;
g_ui.GetSettingBool("acme.mymod", "enabled", &enabled);

std::int64_t count = 0;
g_ui.GetSettingInt("acme.mymod", "count", &count);

double scale = 0.0;
g_ui.GetSettingFloat("acme.mymod", "scale", &scale);
```

`GetSettingString` handles string, enum (the option), and key (the key name,
e.g. `"F10"`). It returns the length including the NUL, and always
NUL-terminates. Pass a null buffer to ask how big:

```cpp
std::uint32_t need = g_ui.GetSettingString("acme.mymod", "mode", nullptr, 0);
std::string buf(need ? need - 1 : 0, '\0');
if (need) g_ui.GetSettingString("acme.mymod", "mode", buf.data(), need);
```

(`type:"flags"` is a JSON array — no typed getter; read it from the change
callback.)

Subscribe to a mod's values. It's per-mod, not per-key — switch on the key. The
current values are replayed once on subscribe, so you don't need a separate
initial read.

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

On a host older than one of your setting types, the getters and replay serve the
schema default. The user's saved value stays on disk and comes back when they
upgrade.

Register a schema at runtime instead of shipping a file (same JSON):

```cpp
if (!g_ui.RegisterSettingsSchema(schemaJson)) {
    // false = bad JSON, non-object, or missing/invalid "id"
}
g_ui.UnregisterSettingsSchema("acme.mymod");   // keeps the user's saved values
```

User values overlay from the same file as the drop-in tier, so you can migrate
from a file to a runtime registration without losing settings.

### 5b. Hotkeys

ABI 1.4 (`Feature::kHotkeys`).

Fires when the key currently bound to a key-typed setting is pressed. You
subscribe to the setting, not a key code — OSF UI re-resolves it on every
rebind.

```cpp
void OnHotkey(const char* modId, const char* key, void* user) noexcept
{
    // Main thread. A hotkey is an event, not state — no replay.
}

std::uint32_t token = g_ui.SubscribeHotkey("acme.mymod", "openKey", &OnHotkey, nullptr);
g_ui.UnsubscribeHotkey(token);
```

Doesn't fire while the overlay is capturing text or during a rebind, and key
repeats don't fire. Conflicting bindings across mods all fire — the settings UI
flags conflicts but never blocks them.

### 5c. Views

ABI 1.5 (`Feature::kRegisterView`).

Validates a discovered `views/<modId>/<viewName>/` folder your mod ships,
without the user's `config.json` listing it. The view appears in the Mods
surface and responds to `RequestMenu` and the web `menu.open`; its WebView2
page is created only when first opened unless its manifest has
`openOnStart:true`.

```cpp
g_ui.RegisterView("acme.mymod/dashboard");                            // validate registration; page stays lazy
g_ui.SendToWeb("acme.mymod/dashboard", "acme.mymod.state", "{...}");  // optional pre-state
g_ui.RequestMenu("acme.mymod/dashboard", true);                       // open
```

Issue all three back-to-back from any thread — they apply in order on the same
tick, and (§6a) the page sees the state message before its first paint.

- Idempotent — an already-live view isn't reloaded.
- A missing folder just warns (ship the folder with your mod).
- `openOnStart` from the manifest creates and opens the view immediately.
- Returns false only on a null/invalid id.

`RegisterView` is an optional declaration for a **plugin-shipped** folder. A
plain drop-in view is found at boot and loads on first open with no plugin at
all.

### 5d. Session health

ABI 1.7 (`Feature::kDiagnostics`).

Report a condition into OSF UI's **System Health** pane — the one place a player
looks when something is wrong, whichever mod noticed it. Don't build a second
health page in your own view: a player who has to know which mod broke before
they know where to look has the problem backwards.

```cpp
constexpr const char* kMod = "acme.mymod";

// Something is wrong, and stays wrong until it isn't.
g_ui.ReportIssue(kMod, "pack-parse:highlights", "catalog.parse-failed",
                 OSFUI::API::IssueSeverity::kError, "highlights",
                 R"({"file":"highlights.json","line":12})");

// It cleared.
g_ui.ClearIssue(kMod, "pack-parse:highlights");
```

**This is not a log channel and not a toast.** Report only what is *durable*
(still true when the player reads it), *actionable* (they can do something), and
*worth interrupting them for*. Routine progress, one-frame hiccups, and anything
that has already corrected itself belong in your log. A pane full of noise is a
pane players learn to skip.

Identity, not events:

- `id` is **your** dedupe key. Re-reporting a live id bumps its occurrence count
  in place — that is what tells "once at startup" from "every few seconds" —
  instead of stacking cards.
- `code` is **your** stable machine code for the *kind* of condition. Never
  prose: OSF UI owns the wording so it stays localizable and no mod can write the
  words on its own card. A code OSF UI doesn't know renders as a card naming your
  mod with your context shown as technical detail — degraded, never broken.
- `ClearIssue` moves it to **Resolved this session**, which is exactly what a
  player wants to see after a retry. Cheap to call unconditionally.
- Recomputing a whole set? Report what's wrong now, then sweep:
  `ClearIssuesExcept(kMod, R"(["still-bad-1","still-bad-2"])")`. FIFO with
  `ReportIssue`, so the pair lands correctly in one tick. **The sweep is scoped to
  your mod, not to one producer inside it** — if your plugin reports from several
  places, the keep list must name every id you still want live, or the others are
  withdrawn as collateral. Track what you've raised.

Everything is namespaced to the calling mod: the issue's `source` is **your mod
id, assigned by the host** — never a parameter — and your ids and codes are
prefixed with it. Two mods can use the same local id without colliding, and no
mod can resolve or overwrite a platform issue.

`context` is optional bounded detail: a **flat JSON object** of string / number /
bool values, capped at 8 entries and 240 chars per value. It is sanitized on the
way in — anything path-, URL-, or command-shaped is cut to its trailing
component, because an absolute path identifies the player's machine and account.
Pass bare filenames and ids; don't rely on nested values surviving.

On a host older than 1.7 all three are no-ops returning false. Reporting your
health unconditionally is safe — you just have nowhere to report to.

---

## 6. Native → web messaging

### 6a. SendToWeb

Sends `{ "type": type, "payload": payloadJson }` to one view. `payloadJson` must
be valid JSON. It arrives at that view's `osfui.onMessage`.

With the optional facade, pass JSON or any `nlohmann`-convertible struct:

```cpp
OSFUI::API::JsonClient jsonUi{ g_ui };
jsonUi.SendToWeb("acme.mymod/dashboard", "acme.mymod.state", {
    { "hp", 42 },
    { "credits", 1000 }
});
```

The dependency-free low-level call remains available:

```cpp
g_ui.SendToWeb("acme.mymod/dashboard", "acme.mymod.state",
               "{\"hp\":42,\"credits\":1000}");
```

`JsonClient` also accepts JSON directly for `RegisterSettingsSchema`,
`ReportIssue`, and `ClearIssuesExcept`, removing every manual `dump()` lifetime
from the public API's JSON-bearing calls.

**Delivery guarantee (ABI 1.3).** A message to a known view is held or queued
until that target can receive it. Lazy or idle-reclaimed targets retain it on
the game side; a loading page retains it in the renderer; a suspended page is
resumed for delivery. The queue flushes FIFO before the view's first visible
paint after `RequestMenu(view, true)`. So:

```cpp
g_ui.SendToWeb(v, "acme.mymod.state", "{...}");
g_ui.RequestMenu(v, true);
```

opens the view already in the right state — no flash of default content.

Queues are bounded (drops oldest, logs a warning), so a view that never opens
can't leak memory. `SendToWeb` returns false only on null args or bad JSON.

**Open/close a view:** `RequestMenu(view, open)` (ABI 1.1). Opening a discovered
folder loads it on demand. Returns true if an open target exists and the request
was queued; false if no such view was found. Closing works only on a loaded
view. True doesn't promise the page renders.

---

## 7. Threading & lifetime

**Threading**

- Any thread, synchronous: all status reads and the typed setting getters.
- Any thread, applied next tick: every mutating call (register, send, subscribe,
  request menu, etc.).
- Always the main thread: every callback (`CommandFn`, `RequestFn`, `ReadyFn`,
  `SettingChangedFn`, `HotkeyFn`). Keep them cheap.

**Lifetime**

- `const char*` args passed **into** callbacks are valid only during the call.
  Copy anything you keep.
- Callbacks can fire for the whole process — registrations survive bridge
  re-creation. Don't point one at something you might free; use static/leaked
  state, or unregister first.
- Settings replay can deliver the same value twice. Make `SettingChangedFn`
  idempotent.
- Strings returned by the API are static, valid for the process.
- OSF UI owns the bridge; never delete it.

---

## 8. Method reference

All on `IOSFUIBridge`, mirrored on `Client` (which adds the version gate).

| Method | ABI | Thread | Notes |
|---|---|---|---|
| `GetInterfaceVersion()` | 1.0 | any | packed `(major<<16)｜minor` |
| `GetPluginVersion(maj,min,pat)` | 1.0 | any | OSF UI release |
| `GetBridgeProtocolVersion()` | 1.0 | any | don't parse |
| `IsBridgeReady()` | 1.0 | any | a view is live |
| `RegisterCommand(cmd,fn,user)` | 1.0 | any | shape `<author>.<modname>.<name>` (1.6) |
| `UnregisterCommand(cmd)` | 1.0 | any | |
| `SendToWeb(view,type,json)` | 1.0 | any | queued; delivery guarantee at 1.3 |
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
| RegisterRequest(name,fn,user) | 1.7 | any | first-wins across commands and requests; callback on main |
| UnregisterRequest(name) | 1.7 | any | in-flight tokens remain valid until answer/timeout/close |

---

## 9. Example plugin

Surfaces its own view, seeds it with state, reacts to a setting, and answers a
command. Never hard-fails when OSF UI is absent.

```cpp
#include "OSFUI_API.h"
using namespace OSFUI::API;

static Client g_ui;

static void OnRefresh(const char*, const char*, const char* srcView, void*) noexcept
{
    g_ui.SendToWeb(srcView, "acme.mymod.state", "{\"credits\":1000}");
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
    if (!g_ui.Init()) return;   // OSF UI absent — degrade silently

    g_ui.RegisterCommand("acme.mymod.refresh", &OnRefresh, nullptr);

    if (g_ui.Has(Feature::kSettings)) {
        g_ui.SubscribeSettings("acme.mymod", &OnSetting, nullptr);
    }

    if (g_ui.Has(Feature::kRegisterView)) {
        g_ui.RegisterView("acme.mymod/dashboard");
        g_ui.SendToWeb("acme.mymod/dashboard", "acme.mymod.state", "{\"credits\":1000}");
        g_ui.RequestMenu("acme.mymod/dashboard", true);
    }
}
```

---

## 10. See also

- [`sdk/OSFUI_API.h`](../sdk/OSFUI_API.h) — the header (source of truth).
- [`sdk/README.md`](../sdk/README.md) — SDK overview.
- [authoring-views.md](authoring-views.md) — the view (JS) side, `window.osfui`.
- [authoring-settings.md](authoring-settings.md) — settings schemas and reading them.
- [security-model.md](security-model.md) — where native command registration sits in the trust model.
