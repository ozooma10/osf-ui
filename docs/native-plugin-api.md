# Native plugin bridge API — design / RFC

**Status:** Implemented (OSF UI side) — `src/api/{BridgeApi.{h,cpp},Exports.cpp}`
+ `sdk/OSFUI_API.h`; `OSFUI_RequestBridge` verified exported; builds clean
(2026-07-01). ABI **1.1** added `RequestMenu`; ABI **1.2** added the settings
surface (`SubscribeSettings`, typed getters, `RegisterSettingsSchema` —
2026-07-13, mcm-design.md §8.2); ABI **1.3** strengthened `SendToWeb` into a
delivery guarantee (no vtable change); ABI **1.4** added hotkey dispatch
(`SubscribeHotkey` — 2026-07-13, mcm-design.md §9). Consumer (OSF Animation)
side pending. ·
**Originally:** Proposed 2026-06-30 · **Brand:** OSF UI (no Prisma-compat
constraint)

This document specifies a public, ABI-stable C++ API that lets a **separate
SFSE plugin** register bridge commands, push data to a web view, and receive
commands back from it — without compiling its code into `OSFUI.dll`. It is the
seam `runtime/UiModule.h:11-12` calls out as "a future public plugin API … so
modules can ship in separate DLLs," and it supersedes the short-lived
Prisma-source-compatible `RequestPluginAPI` experiment (added 2026-06-24,
reverted 2026-06-25; see `docs/ROADMAP.md`). Per that roadmap entry, this is
designed **under the OSF UI brand** with no Prisma compatibility requirement.

The motivating first consumer is **OSF Animation**, which will be the flagship
view (scene browser/launcher) but lives in its own DLL and owns its own scene
registry and runtime.

---

## 1. Summary

Today OSF UI is a single self-contained runtime. The native↔web `MessageBridge`
is feature-agnostic, but **everything that registers a command or pushes data is
compiled into `OSFUI.dll`** (`Runtime::RegisterPlatformCommands` +
`IUiModule::RegisterCommands`, wired in `Runtime::BuildModules`). A page can only
invoke a fixed whitelist; there is no generic "call native" hatch
(`docs/security-model.md`), and there is no exported symbol, no `.def`, and no
inter-plugin messaging path for a sibling DLL to extend any of it.

This API adds exactly one thing: a way for another **trusted native plugin** to
add its own `ui.command` handlers and `SendToWeb` channel at runtime. It does
**not** widen what untrusted JS can do on its own — a page still reaches only
commands some native owner chose to register.

The single most important design rule:

> **The cross-DLL surface carries only JSON text, view ids, primitives, and
> function pointers — never STL types, `nlohmann::json`, or engine (`RE::*`)
> types by value or layout.**

That makes the API ABI-bulletproof and, notably, **independent of the
CommonLibSF pin**: two plugins built against different commonlibsf commits can
still talk over it, because nothing layout-sensitive crosses the boundary. (The
scene work OSF Animation does in response to a command happens *in its own
process* against *its own* commonlibsf, so no engine pointers ever cross.)

---

## 2. Goals / non-goals

**Goals (v1)**

- A sibling plugin can `RegisterCommand("ns.foo", handler)` and have the page's
  `ui.command` `{command:"ns.foo"}` reach it.
- A sibling plugin can `SendToWeb(viewId, type, payloadJson)` to push data/events
  into a view.
- Robust ordering: registration works **before** the bridge exists and survives
  bridge (re)creation — a deferred, idempotent registry applied on the main
  thread.
- ABI stability via a versioned, append-only vtable + a single copyable header,
  mirroring OSF Animation's `OSF_RequestSceneAPI` pattern
  (`src/API/OSFSceneAPI.h` in that repo).
- Thread-safety: all calls are safe from any thread; effects land on the game
  main thread (where bridge handlers must run).

**Non-goals (v1) — explicitly deferred**

- Programmatic **view registration** from an arbitrary path. v1 reuses the
  existing disk-discovery (`ViewManager` scans `OSFUI/views/*`); the consumer
  ships a view folder there. A `RegisterViewRoot` extension is sketched in §9.
- Direct **JS eval / fast interop** (`Invoke`/`InteropCall`) and a **DevTools**
  surface — these are Prisma-parity items parked in `docs/ROADMAP.md`; the
  internal renderer primitives exist but stay internal for now.
- Gamepad/controller, localization, network — unrelated and out of scope.

---

## 3. Background: the integration points this builds on

All real and in-tree today:

| Concern | Where | Note |
|---|---|---|
| Command registry | `MessageBridge::RegisterCommand(std::string, CommandHandler)` `MessageBridge.h:29` | exact-match `unordered_map`; last-write-wins |
| Handler type | `CommandHandler = std::function<void(const nlohmann::json&, MessageBridge&)>` `MessageBridge.h:23` | internal; **not** ABI-safe to expose |
| Reply target | `MessageBridge::CurrentSource()` `MessageBridge.h:48` | source view of the in-flight command |
| Native→web | `MessageBridge::SendToWeb(viewId, type, payload)` `MessageBridge.h:40` | enqueues onto the view's bounded `toWeb` queue |
| Bridge owner / gate | `Runtime::Bridge()` `Runtime.h:92`; built in `Initialize` only when a `nativeBridge` view is present | `_bridge` is null otherwise |
| Main-thread cadence | `Runtime::Tick(dt)` via the `FrameTickTask` permanent task `core/Plugin.cpp:23-56,45` | every frame, under SFSE's queue lock — keep cheap |
| SFSE lifecycle | `OnLoad` registers the listener; `Runtime::Initialize` runs at `SFSE_PLUGIN_LOAD` `core/Plugin.cpp:128-159` | API object must exist by `kPostLoad` |
| Protocol version | `kBridgeProtocolVersion = "0.4"` `core/Version.h` | the web-message contract version, distinct from plugin version |

The API is a thin, ABI-safe adapter in front of `MessageBridge::RegisterCommand`
and `SendToWeb`, plus a deferred-registration layer so a sibling can register at
any time.

---

## 4. Discovery & versioning

A single undecorated C export, fetched once via module + symbol lookup — exactly
the shape OSF Animation already ships (`OSF_RequestSceneAPI`):

```cpp
extern "C" __declspec(dllexport)
OSFUI::API::IOSFUIBridge* OSFUI_RequestBridge(std::uint32_t a_abiVersion) noexcept;
```

- **Module:** `OSFUI.dll` (install layout: `Data/SFSE/Plugins/OSFUI.dll`).
- **When:** the consumer requests it after SFSE `kPostLoad` (OSF UI's
  `Runtime::Initialize` has already run at `SFSE_PLUGIN_LOAD`, so the singleton
  exists). Fetch once and cache — never per-frame.
- **Version:** packed `(MAJOR << 16) | MINOR`, currently `(1 << 16) | 4`.
  - **MAJOR** breaks ABI (reordered/removed vmethod, changed signature). The
    export returns `nullptr` if the caller's MAJOR ≠ OSF UI's MAJOR.
  - **MINOR** bumps when a vmethod is **appended** to the end of the vtable —
    or (1.3) when a behavioral guarantee is strengthened with no vtable change.
    Older callers (lower MINOR) keep working; they simply never call the new
    tail methods. History: **1.0** commands/sends/ready · **1.1** `RequestMenu`
    · **1.2** settings (`SubscribeSettings`/`UnsubscribeSettings`, typed
    getters, `RegisterSettingsSchema`/`UnregisterSettingsSchema`) · **1.3**
    `SendToWeb` delivery guarantee (§6a) · **1.4** hotkeys
    (`SubscribeHotkey`/`UnsubscribeHotkey`).
  - **Feature detection:** an older 1.x `OSFUI.dll` accepts any 1.x caller, so
    a consumer built against a newer header can receive a bridge whose vtable
    ends early. Gate calls to a later MINOR's methods on the runtime's version:
    `(bridge->GetInterfaceVersion() & 0xFFFF) >= 2` before touching the
    settings surface, `>= 4` before the hotkey surface.
- **Web protocol version** is separate: `GetBridgeProtocolVersion()` returns the
  version string from `core/Version.h` (currently `"0.4"`) so a consumer can
  gate its **JS message contract** independently of the C++ ABI.

`RequestBridge` returning `nullptr` (OSF UI absent, or MAJOR mismatch) is a
normal, expected outcome — the consumer degrades (e.g. no UI) rather than
failing.

---

## 5. The interface

ABI-safe by construction: every parameter is a primitive, a `const char*`
(UTF-8, null-terminated, valid only for the duration of the call), a function
pointer, or `void*` user data. No STL, no `nlohmann::json`, no `RE::*`.

```cpp
namespace OSFUI::API
{
    // Handler for one registered ui.command. Runs on the GAME (main) thread.
    //   a_command      : the command string registered (lets one fn serve many)
    //   a_payloadJson  : the command payload object, serialized — e.g. "{\"id\":\"x\"}"
    //   a_sourceViewId : the view that sent it (your reply target)
    //   a_user         : the opaque pointer you passed to RegisterCommand
    using CommandFn = void (*)(const char* a_command,
                               const char* a_payloadJson,
                               const char* a_sourceViewId,
                               void*       a_user) noexcept;

    // Fired on the GAME (main) thread when the bridge becomes ready (a
    // nativeBridge view is live) and again after any re-creation. (Re)send
    // initial state from here.
    using ReadyFn = void (*)(void* a_user) noexcept;

    // 1.2 — fired on the GAME (main) thread for every committed value of a mod
    // subscribed via SubscribeSettings. a_valueJson is serialized JSON text
    // ("true", "1.5", "\"compact\"").
    using SettingChangedFn = void (*)(const char* a_modId, const char* a_key,
                                      const char* a_valueJson, void* a_user) noexcept;

    // 1.4 — fired on the GAME (main) thread when the physical key currently
    // bound to the subscribed key-typed setting is pressed (mcm-design.md §9).
    using HotkeyFn = void (*)(const char* a_modId, const char* a_key,
                              void* a_user) noexcept;

    struct IOSFUIBridge
    {
        // --- versioning / status. ANY thread, synchronous. ---
        virtual std::uint32_t GetInterfaceVersion() = 0;
        virtual void          GetPluginVersion(std::uint32_t& a_major,
                                               std::uint32_t& a_minor,
                                               std::uint32_t& a_patch) = 0;
        virtual const char*   GetBridgeProtocolVersion() = 0;  // web protocol, e.g. "0.1"
        virtual bool          IsBridgeReady() = 0;             // a nativeBridge view is live

        // --- command registration. Thread-safe; applied on the next main tick. ---
        // Register/replace the handler for an EXACT command string (e.g. "osf.launch").
        // Persists across bridge re-creation. Reserved prefixes are refused
        // (see §7): ui. / runtime. / game. / settings. / views. Use your own namespace.
        virtual void RegisterCommand(const char* a_command, CommandFn a_handler, void* a_user) = 0;
        virtual void UnregisterCommand(const char* a_command) = 0;

        // --- native -> web. Thread-safe; queued to the target view. ---
        // Delivers { "type": a_type, "payload": <a_payloadJson> } to a_viewId.
        // a_payloadJson must be valid JSON text. Returns false if the bridge is
        // down or the payload won't parse (message dropped). Delivery to an
        // unknown/closed view is best-effort and logged (like today).
        virtual bool SendToWeb(const char* a_viewId, const char* a_type, const char* a_payloadJson) = 0;

        // --- readiness notification. Callback runs on the main thread. ---
        virtual void SetReadyCallback(ReadyFn a_callback, void* a_user) = 0;

        // --- 1.1: menu control. Thread-safe; applied on the next main tick. ---
        // Open/close a registered SURFACE by id through the normal menu policy.
        virtual bool RequestMenu(const char* a_viewId, bool a_open) = 0;

        // --- 1.2: settings consumption (mcm-design.md §8.2). ---
        // Per-mod change subscription; callbacks on the GAME MAIN thread, with
        // a replay of current values on subscribe (see §5a).
        virtual std::uint32_t SubscribeSettings(const char* a_modId, SettingChangedFn a_fn, void* a_user) = 0;
        virtual void          UnsubscribeSettings(std::uint32_t a_token) = 0;
        // Typed getters: synchronous, ANY thread (they read a mutex-guarded
        // mirror, never the store). false / 0 on unknown mod/key or mismatch.
        virtual bool GetSettingBool (const char* a_modId, const char* a_key, bool* a_out) = 0;
        virtual bool GetSettingInt  (const char* a_modId, const char* a_key, std::int64_t* a_out) = 0;
        virtual bool GetSettingFloat(const char* a_modId, const char* a_key, double* a_out) = 0;
        // Covers string/enum/key. Returns required length incl. NUL; copies min(bufLen).
        virtual std::uint32_t GetSettingString(const char* a_modId, const char* a_key,
                                               char* a_buf, std::uint32_t a_bufLen) = 0;

        // --- 1.2: settings registration. Thread-safe; merge on the next main tick. ---
        // The SAME JSON document as a settings/<id>.json drop-in. Parse/shape
        // errors report synchronously (false); see §5a for precedence.
        virtual bool RegisterSettingsSchema(const char* a_schemaJson) = 0;
        virtual void UnregisterSettingsSchema(const char* a_modId) = 0;

        // --- 1.4: hotkey dispatch (mcm-design.md §9). Thread-safe; callbacks
        // on the GAME MAIN thread. Per-(mod, key); see §5b.
        virtual std::uint32_t SubscribeHotkey(const char* a_modId, const char* a_key,
                                              HotkeyFn a_fn, void* a_user) = 0;
        virtual void          UnsubscribeHotkey(std::uint32_t a_token) = 0;

    protected:
        ~IOSFUIBridge() = default;  // OSF UI owns the singleton; the consumer never deletes it.
    };
}
```

### Threading contract

- **Status reads** (`GetInterfaceVersion`/`GetPluginVersion`/
  `GetBridgeProtocolVersion`/`IsBridgeReady`) return synchronously from any
  thread (atomic/const).
- **Typed setting getters** (`GetSettingBool/Int/Float/String`) are synchronous
  and callable from **any** thread: they read a mutex-guarded value mirror that
  a store listener maintains on the main thread (`src/api/SettingsMirror.*`),
  never `SettingsStore` itself.
- **Mutating calls** (`RegisterCommand`/`UnregisterCommand`/`SetReadyCallback`/
  `SendToWeb`/`RequestMenu`/`SubscribeSettings`/`RegisterSettingsSchema`/…) are
  safe from any thread; their effect is marshaled onto the next `Runtime::Tick`
  on the main thread. This mirrors how everything else in the plugin already
  gets main-thread cadence (`core/Plugin.cpp:45`), and means a consumer can
  push data from a worker thread without its own marshaling.
- **`CommandFn` / `ReadyFn` / `SettingChangedFn` / `HotkeyFn` always run on the
  game main thread.** Keep them cheap (same discipline as `Tick`); offload
  heavy work yourself.

### 5a. Settings surface semantics (ABI 1.2, mcm-design.md §8.2)

- **Subscription is per-mod, not per-key** — one subscription, switch on
  `a_key`. On subscribe, the mod's **current values replay** once each (on the
  next main tick), so no separate initial read is needed. Subscribing to a
  not-yet-registered mod id is legal; the replay arrives when it registers
  (load-order insurance). A value committed in the subscribe→first-tick window
  can be delivered twice with identical content — callbacks should be
  idempotent per `(mod, key, value)`.
- **Getters judge type by the stored value's JSON shape:** `GetSettingBool`
  wants a bool, `GetSettingInt` an integral number, `GetSettingFloat` any
  number, `GetSettingString` a string (covers `string`, `enum` — the option
  string — and `key` — the key NAME, e.g. `"F10"`). `GetSettingString` returns
  the required length **including** the NUL (0 on unknown/mismatch), copies
  `min(bufLen)`, always NUL-terminates; a null/empty buffer is the "how big?"
  probe.
- **`RegisterSettingsSchema` takes the exact drop-in document**
  (`settings/<id>.json` shape, `docs/schema/`). Malformed JSON, a non-object
  document, or a missing/invalid/reserved `id` fail **synchronously** (false);
  deeper field problems fall back defensively exactly like a drop-in file.
  Persisted user values overlay from the same per-mod values file as the
  drop-in tier, so a mod can migrate tiers without losing settings. Same id as
  a drop-in file: the runtime registration **wins** (logged); re-registering
  the same id replaces the earlier registration (dev iteration). Subscribers
  of the id receive the value replay when the merge commits on the main tick.
- **`UnregisterSettingsSchema` only drops runtime-registered schemas** — it is
  ignored (with a log warning) for ids owned by drop-in files. The user's
  values file is always kept.

### 5b. Hotkey surface semantics (ABI 1.4, mcm-design.md §9)

Every `type:"key"` setting of every registered mod is a **live, dispatchable
binding**: OSF UI's central `HotkeyService` resolves each one's current value
to a physical key and routes presses to the setting's owner. There is no
schema flag — **subscription is the delivery opt-in**.

- **Subscription is per-(mod, key)** — subscribe once per hotkey you own.
  `SubscribeHotkey("myMod", "toggleHud", fn, user)` returns a token (0 on
  null/empty args); `UnsubscribeHotkey(token)` stops delivery immediately,
  including when called from inside a callback. Subscribing to a setting that
  doesn't exist (yet) or isn't key-typed is legal — it just never fires until
  such a binding dispatches.
- **The binding is whatever the user set.** OSF UI rebuilds its registry on
  every key-typed commit (a rebind through the settings UI, an ABI write, a
  reset) and on registry shape changes — consumers never see or track VK
  codes, only their own `(mod, key)`.
- **Context-gated, so you never double-fire:** a press while the overlay
  captures input (the user is typing into a view) or while a rebind capture is
  armed does **not** fire; key repeats don't fire; the down edge fires once.
  This is the reason to use this service instead of your own raw input hook —
  your hook can't see OSF UI's capture state.
- **Never consumed, never exclusive:** the game still receives the key, and
  duplicate bindings across mods **all fire**. Collisions are surfaced to the
  user in the settings UI (informational `conflicts` data in `settings.data`),
  never rejected.
- **Web-side delivery exists too:** the owning mod's views get a
  `ui.hotkey {mod, key}` push if they subscribed via `settings.get` — a HUD
  can "toggle myself" with zero native code (`docs/authoring-views.md`). Use
  `SubscribeHotkey` when the *reaction* is native; you don't need both.

---

## 6. Lifecycle & the ready gate (the load-bearing robustness piece)

`_bridge` exists only after `Runtime::Initialize` **and** only if a
`nativeBridge` view loaded. A consumer may `RequestBridge` before that, and its
view's DOM may not be ready yet. So the API keeps its **own** registry,
independent of `MessageBridge` lifetime:

1. `RequestBridge` returns the singleton `BridgeApi` immediately (valid from
   `kPostLoad`), even if no bridge exists yet.
2. `RegisterCommand` records `{command → (fn, user)}` in the API's own map and
   enqueues an "apply" op.
3. On `Runtime::Tick`, the API drains its op queue on the main thread. When a
   live `MessageBridge` is present, it (re)applies every recorded command via a
   `MessageBridge::RegisterCommand` **trampoline** (see §8) and fires the ready
   callback once.
4. If the bridge is later re-created, the API re-applies the whole registry —
   the consumer never re-registers.
5. `SendToWeb` before the bridge is up is **queued** (ABI 1.3; on ≤ 1.2 it
   returned `false`/dropped): the pump flushes it FIFO once a bridge appears,
   and the renderer stashes it per view until that view's page can receive
   (loaded + `osfui.onMessage` installed). Queues are bounded per view
   (drop-oldest, warned in the log). Pushing initial state from the `ReadyFn`
   callback is still good practice; it is just no longer load-bearing.

### 6a. Delivery guarantee: open a view in a target state (ABI 1.3)

A message to a loaded, bridge-enabled view is never dropped for timing
reasons, and queued messages are delivered **before the view's first visible
paint** after a `RequestMenu(viewId, true)`. The canonical consumer sequence

```cpp
g_ui->SendToWeb("osf", "osf.mode", R"({"mode":"wheel"})");
g_ui->RequestMenu("osf", true);
```

guarantees the page's JS observes `osf.mode` before the view is on screen — no
flash of the page's default face. Three host mechanisms compose into this:

- `Runtime::Tick` snapshots `RequestMenu` ops **before** the bridge pump
  flushes queued sends and applies them **after** it, so a send issued before
  an open is always in the view's queue first.
- The renderer's worker snapshots each view's hidden flag in the same critical
  section that takes its message queue, and delivers queued messages before it
  renders/composites — a view never appears in output produced by a pass that
  had an undelivered message for it (`IsPresentable`,
  `UltralightWebRenderer.cpp`).
- On the closed→open edge the compositor reveal is deferred until the renderer
  hands over a frame produced after the open (`Runtime::SubmitFrameIfVisible`),
  so the present hook can't re-draw stale pre-open overlay content. Costs at
  most a couple of frames of open latency.

Detect the guarantee with `(GetInterfaceVersion() & 0xFFFF) >= 3` — e.g. OSF
Animation gates removing its "veil" (hide-until-first-mode-push workaround) on
it. Messages to a **visible, ready** view are unaffected: same queue, same
next-worker-pass delivery as before.

**A view is still required.** Requesting the API does not conjure a bridge. The
consumer must ship a view with `permissions.nativeBridge: true` (that view is
what triggers `_bridge` construction). If the view fails to load, `IsBridgeReady`
stays false and `ReadyFn` never fires — surface that to the user.

SFSE has **no shutdown callback** (`core/Plugin.cpp:155-157`); the singleton has
process lifetime. `UnregisterCommand` exists for hot cleanup, but a consumer
cannot rely on being unloaded — handlers must tolerate being called any time
after registration.

---

## 7. Security model

This API is a **trusted-native-plugin** surface — a different trust tier than
the untrusted-JS rule in `docs/security-model.md`. A plugin that can register
commands is already running native code in the process; the API gives it no
capability it didn't already have. Crucially, it does **not** widen the JS
attack surface: a page still reaches only commands a native owner registered, and
still cannot call arbitrary native code.

Guards the implementation enforces:

- **Reserved prefixes.** `RegisterCommand` refuses (logs + ignores) any command
  beginning `ui.`, `runtime.`, `game.`, `settings.`, or `views.` — the
  platform/first-party namespaces. Consumers use their own (`osf.*`).
- **Collision logging.** Re-registering an existing command logs a warning with
  both owners; last-write-wins is preserved (matches `MessageBridge` today).
- **Guaranteed-order delivery, bounded.** `SendToWeb` rides the per-view
  bounded `toWeb` queue (plus a bounded pre-ready queue in `BridgeApi` and a
  bounded per-view stash in the renderer); overflow drops the OLDEST with a
  log warning, so a spammed queue still converges on the newest state.
  Handlers and senders must stay cheap (main-thread, under the task lock).
- **No path/network capability** is added. The sandbox
  (`UltralightWebRenderer.cpp` `SandboxFileSystem`) is unchanged; views still
  cannot read outside `OSFUI/views`. (This is *why* the API exists: a view
  cannot read another mod's data folder, so the catalog must arrive over this
  bridge.)

`docs/security-model.md` should gain a short section noting the native API as a
trusted, command-scoped extension point.

---

## 8. Implementation in OSF UI

New, self-contained, mirroring OSF Animation's `src/API/` layout:

```
src/api/BridgeApi.h     // the BridgeApi singleton (internal impl of IOSFUIBridge)
src/api/BridgeApi.cpp
src/api/Exports.cpp     // extern "C" __declspec(dllexport) OSFUI_RequestBridge
sdk/OSFUI_API.h         // the public single header consumers copy (Appendix A)
```

Wire-in points (small, localized):

- **`Exports.cpp`** — the factory does the MAJOR check and returns
  `&BridgeApi::Get()` (or `nullptr` on mismatch). One TU; no change to the
  `SFSE_PLUGIN_*` macros in `src/main.cpp`.
- **`BridgeApi`** holds: the external command registry
  (`command → {CommandFn, user}`), a main-thread op queue, the ready callback,
  and a non-owning `MessageBridge*` (set by Runtime). It is the concrete
  `IOSFUIBridge`.
- **`Runtime::Initialize`** — after the existing bridge/module wiring
  (`Runtime.cpp:127-130`), hand the live bridge to the API and flush:
  `API::BridgeApi::Get().OnBridgeReady(_bridge.get());`. When `_bridge` is null
  (no `nativeBridge` view), pass `nullptr` so the API stays not-ready.
- **`Runtime::Tick`** — drain the API's op queue:
  `API::BridgeApi::Get().PumpMainThread();` (applies pending registrations and
  off-thread `SendToWeb`s). One cheap call per frame.

The **trampoline** that adapts the ABI-safe `CommandFn` to the internal
`CommandHandler` (this is the only place the two worlds meet):

```cpp
// BridgeApi::ApplyTo — called on the main thread when a bridge is live.
void BridgeApi::ApplyTo(MessageBridge& a_bridge)
{
    for (const auto& [cmd, reg] : _commands) {
        a_bridge.RegisterCommand(cmd,
            [reg, cmd](const nlohmann::json& a_payload, MessageBridge& a_b) {
                const std::string dump = a_payload.dump();          // json -> text
                const std::string src(a_b.CurrentSource());          // reply target
                reg.fn(cmd.c_str(), dump.c_str(), src.c_str(), reg.user);
            });
    }
    _ready = true;
    if (_readyCb) _readyCb(_readyUser);
}

// BridgeApi::SendToWeb — main-thread body (off-thread calls are queued to here).
bool BridgeApi::SendToWebMain(std::string_view a_view, std::string_view a_type,
                              std::string_view a_payloadJson)
{
    if (!_bridge) return false;
    auto payload = nlohmann::json::parse(a_payloadJson, nullptr, /*allow_exceptions*/ false);
    if (payload.is_discarded()) return false;
    _bridge->SendToWeb(a_view, a_type, payload);
    return true;
}
```

Optional minor `MessageBridge` nicety (not required for v1): a raw-string
`SendToWeb(view, type, std::string_view rawJson)` overload to skip the
parse→re-dump round-trip, and making the renderer's `SendMessageToWeb` return a
`bool` so `SendToWeb` can report "unknown view" precisely instead of best-effort.

**ABI / commonlibsf:** because the API surface carries no `RE::*` or STL types,
the OSF UI ⇄ consumer boundary is **independent of the commonlibsf pin**. The
fast-forward of `lib/commonlibsf` (`5df499f` → OSF Animation's `8fdcac4`, a
clean same-branch FF) is therefore **not** required for this bridge. It only
matters if some future API passes engine types (don't), or for the consumer's
*own* in-process engine work.

---

## 9. Future: programmatic views (`RegisterViewRoot`) — v1.1

v1 requires the consumer to install its view into `OSFUI/views/<id>/` (OSF UI's
own data dir). That works but couples distribution. A clean follow-up lets each
mod ship its view under **its own** folder:

```cpp
// v1.1 — appended to the vtable (MINOR bump).
//   a_absDir : absolute path to a folder containing <id>/manifest.json
// Adds an extra ViewManager scan root AND an extra SandboxFileSystem base so
// that view's pages resolve files under a_absDir/<id> (and nowhere else).
virtual bool RegisterViewRoot(const char* a_absDir) = 0;
```

This requires two real changes flagged for design: `ViewManager::LoadAll` must
accept multiple scan dirs, and `SandboxFileSystem` must support a **per-view**
base instead of the single fixed `_viewsBase` (`UltralightWebRenderer.cpp`
comments "single view root"). Until then, v1's disk-drop is the supported path.

---

## 10. Worked example — OSF Animation as the consumer

OSF Animation links nothing; it copies `OSFUI_API.h`, requests the bridge after
`kPostLoad`, registers its `osf.*` commands, and answers them **in its own
process** (reading its live scene registry, calling its own `SceneRuntime`,
resolving the crosshair via its own commonlibsf). No engine type ever crosses
into OSF UI — only JSON.

```cpp
#include "OSFUI_API.h"
using namespace OSFUI::API;

static IOSFUIBridge* g_ui = nullptr;

// runs on the game main thread
static void OnLaunch(const char*, const char* payloadJson, const char* srcView, void*) noexcept
{
    // parse payloadJson with OSF Animation's own json lib:
    //   { "sceneId": "...", "actors": [tokens], "furnitureToken": n, "opts": {...} }
    // resolve tokens -> RE::Actor* (in-process), call SceneRuntime::Start(...),
    // then reply to the view that asked:
    const std::int32_t handle = /* ... start scene ... */ 0;
    const std::string reply = /* json: {"ok":handle>0,"handle":handle,...} */ "{}";
    if (g_ui) g_ui->SendToWeb(srcView, "osf.launchResult", reply.c_str());
}

static void OnCatalogGet(const char*, const char*, const char* srcView, void*) noexcept
{
    // serialize OSF Animation's LIVE registry (real, registered scenes — not an
    // optimistic disk scan) and push it back:
    const std::string catalog = /* json array of {id,title,tags,actorCount,requiresFurniture,...} */ "[]";
    if (g_ui) g_ui->SendToWeb(srcView, "osf.catalog.data", catalog.c_str());
}

static void OnBridgeReady(void*) noexcept
{
    // a nativeBridge view is live; (re)push anything the page needs up front.
}

// from OSF Animation's SFSE kPostLoad handler:
void HookUpUi()
{
    g_ui = RequestBridge();           // nullptr if OSF UI absent -> skip UI, no error
    if (!g_ui) return;
    g_ui->SetReadyCallback(&OnBridgeReady, nullptr);
    g_ui->RegisterCommand("osf.catalog.get",   &OnCatalogGet, nullptr);
    g_ui->RegisterCommand("osf.launch",        &OnLaunch,     nullptr);
    g_ui->RegisterCommand("osf.stop",          /* ... */ nullptr, nullptr);
    g_ui->RegisterCommand("osf.pickCrosshair", /* ... */ nullptr, nullptr);
}
```

The view side is ordinary OSF UI authoring (`docs/authoring-views.md`): ship
`OSFUI/views/osf/` with `permissions.nativeBridge: true`, then:

```js
window.osfui.onMessage = (json) => {
  const { type, payload } = JSON.parse(json);
  if (type === "runtime.ready")    negotiate(payload.bridgeVersion);   // "0.1"
  if (type === "osf.catalog.data") renderGrid(payload);
  if (type === "osf.launchResult") showResult(payload);
};
const send = (command, args = {}) =>
  window.osfui.postMessage(JSON.stringify({ type: "ui.command", payload: { command, ...args } }));

send("osf.catalog.get");
send("osf.launch", { sceneId, actors, furnitureToken, opts });
```

This replaces the previously-considered Papyrus `OSFUIBridge`: with the native
path, catalog + launch are native-to-native on the main thread, and the catalog
reflects the **live registered** scene set.

---

## 11. Testing

- **Round-trip:** a dev/stub consumer (or a temporary in-tree command) registers
  `dev.echo`; a test view sends it and renders the reply.
- **Ready ordering:** register a command *before* the view's DOM is ready;
  confirm it's applied on `Tick` and the page can call it on first paint.
- **Re-creation:** force a bridge rebuild; confirm registrations re-apply with no
  consumer action.
- **Reserved prefix:** `RegisterCommand("settings.x", …)` is refused + logged.
- **Collision:** two registrations of the same command → warn, last wins.
- **No-bridge `SendToWeb`:** queued (returns true) before any `nativeBridge`
  view loads; flushed FIFO once the bridge appears. If no bridge ever appears,
  the queue stays bounded (per-view cap, drop-oldest, warned).
- **Open-in-state (ABI 1.3):** `SendToWeb(v, mode)` then `RequestMenu(v, true)`
  from a cold start — the page must observe the message before its first
  visible frame (no default-face flash).
- **Off-thread:** `RegisterCommand`/`SendToWeb` from a worker thread land on the
  main thread (assert the handler/sender thread id).
- **Absent OSF UI / MAJOR mismatch:** `RequestBridge` returns `nullptr`; consumer
  degrades cleanly.

---

## 12. Open questions / risks

- **View distribution.** v1 drops the view into `OSFUI/views/`; the clean
  per-mod story needs `RegisterViewRoot` (§9, multi-root scan + per-view
  sandbox). Decide whether v1.1 lands with the first OSF Animation release.
- **Multiple consumers / namespacing.** Flat exact-match registry; rely on the
  reserved-prefix guard + a recommended `mod.*` convention. Revisit if many
  third-party plugins coexist.
- **`SendToWeb` failure granularity.** Without a `bool` from the renderer's
  `SendMessageToWeb`, "unknown view" is best-effort/logged, not reported. Add the
  return value if precise feedback matters.
- **Bridge protocol churn.** `bridgeVersion` is `0.4` and unstable; a MINOR bump
  can break views. Consumers must gate on `GetBridgeProtocolVersion()` /
  `runtime.ready.bridgeVersion` and degrade.
- **Cross-references to update on landing:** `docs/ROADMAP.md` (move "public
  native API" from removed→delivered, scoped to this design),
  `docs/security-model.md` (trusted-native-extension section),
  `docs/authoring-views.md` (note views can be driven by a sibling plugin),
  `sdk/README.md` (link `OSFUI_API.h`).

---

## Appendix A — the copyable header

The authoritative, always-current copyable header is **`sdk/OSFUI_API.h`** in
this repo — copy that file into your SFSE plugin (link nothing). An inline copy
used to live here and drifted from the shipped header (it predated 1.1's
`RequestMenu`); the narrative listing in §5 stays, but the header itself is the
single source of truth for signatures, per-method contracts, and
`kBridgeAPIVersion`.

## Appendix B — message contract for the OSF Animation flagship (informative)

The `osf.*` commands/messages the first consumer will use over this bridge. Not
part of the API surface — the API only transports them — but recorded here so
the two repos agree.

| Direction | `type` / `command` | payload |
|---|---|---|
| web→native | `osf.catalog.get` | — |
| native→web | `osf.catalog.data` | `[{id,title,tags[],actorCount,genders[],requiresFurniture,...}]` |
| web→native | `osf.pickCrosshair` | `{slot:"actor"|"furniture"}` |
| native→web | `osf.pick` | `{slot,token,name,formId,valid}` |
| web→native | `osf.launch` | `{sceneId,mode,castTokens[],roleNames?,furnitureToken?,opts}` |
| native→web | `osf.launchResult` | `{ok,handle,sceneId,error?}` |
| web→native | `osf.stop` | `{handle}` |
| native→web | `runtime.ready` | `{game,plugin,version,bridgeVersion}` (platform; gate on `bridgeVersion`) |
