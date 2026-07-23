# Native plugin API

For SFSE plugin authors. Lets your DLL talk to OSF UI. 
Handle commands from a view, push data to a view, read settings and hotkeys, and open views.

The whole API is one header: [`sdk/OSFUI_API.h`](../sdk/OSFUI_API.h) (C ABI **1.6**). 

Writing a view (HTML/JS) instead? See [authoring-views.md](authoring-views.md) - that's the `window.osfui` side. Your `SendToWeb` lands at a view's `osfui.onMessage`; a view's `osfui.send` lands at your command handler.

**Contents**

- [0. When you need this](#0-when-you-need-this)
- [1. Get the bridge](#1-get-the-bridge)
- [2. Versioning](#2-versioning)
- [3. Commands (web → native)](#3-commands-web--native)
- [4. Status & readiness](#4-status--readiness)
- [5. Settings, hotkeys, and views](#5-settings-hotkeys-and-views)
  - [5a. Settings](#5a-settings)
  - [5b. Hotkeys](#5b-hotkeys)
  - [5c. Views](#5c-views)
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
s
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
| **C ABI** (`1.6`) | `GetInterfaceVersion()` | which native methods exist. Gate on this. |
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

## 5. Settings, hotkeys, and views

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

Loads and registers a `views/<modId>/<viewName>/` folder your mod ships, without
the user's `config.json` listing it. The view then shows up in the Mods surface
and responds to `RequestMenu` and the web `menu.open`.

```cpp
g_ui.RegisterView("acme.mymod/dashboard");                            // load + register
g_ui.SendToWeb("acme.mymod/dashboard", "acme.mymod.state", "{...}");  // optional pre-state
g_ui.RequestMenu("acme.mymod/dashboard", true);                       // open
```

Issue all three back-to-back from any thread — they apply in order on the same
tick, and (§6a) the page sees the state message before its first paint.

- Idempotent — an already-registered view isn't reloaded.
- A missing folder just warns (ship the folder with your mod).
- `openOnStart` from the manifest is honored.
- Returns false only on a null/invalid id.

`RegisterView` is for a **plugin-shipped** folder you want loaded before its
first open. A plain drop-in view is found at boot and loads on first open with
no plugin at all.

---

## 6. Native → web messaging

### 6a. SendToWeb

Sends `{ "type": type, "payload": payloadJson }` to one view. `payloadJson` must
be valid JSON. It arrives at that view's `osfui.onMessage`.

```cpp
g_ui.SendToWeb("acme.mymod/dashboard", "acme.mymod.state",
               "{\"hp\":42,\"credits\":1000}");
```

**Delivery guarantee (ABI 1.3).** A message to a loaded view is queued, never
dropped, while the view can't yet receive it (bridge not live, page loading,
`onMessage` not installed, or view hidden). The queue flushes FIFO before the
view's first visible paint after `RequestMenu(view, true)`. So:

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
- Always the main thread: every callback (`CommandFn`, `ReadyFn`,
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
