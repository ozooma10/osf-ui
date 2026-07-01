# Menu / HUD framework — design & implementation plan

**Status:** Steps 1 + 2 landed in the working tree (builds clean; not yet
in-game verified) · **Date:** 2026-07-01 · **Brand:** OSF UI

This plan replaces the current single-overlay model — one global `_visible` /
`_captureInput` flag toggled by a hardcoded F10 — with **mod-registered surfaces**
that mods open/close on their own triggers, while OSF UI arbitrates ordering,
focus, and the global capture/pause state. It pairs with
[`native-plugin-api.md`](native-plugin-api.md): this doc covers the declarative
(no-code) framework (Steps 1–2); the native `IOSFUISurfaces` tier (Step 3+) is
built on the bridge API that doc specifies.

The design was adversarially verified against the code on 2026-07-01; the
corrections from that pass are folded in and flagged inline as **[verified]**.

---

## 1. Principle — mod owns intent, OSF UI owns mechanism

A mod declares **what** a surface is (a **Menu** or a **HUD**) and decides **when**
it opens/closes (its own hotkey, quest trigger, etc.). OSF UI owns **how**:
compositing, z-order, which single view is focused, and the global
input-capture/pause state — all **derived from the top-of-stack surface every
`Tick`**.

This split is forced by the code, not taste: the dangerous machinery is all
single-instance and RE-fragile — one WndProc subclass (`OverlayInputHook`,
installed once, never un-subclassed), one `_captureInput` atomic
(`Runtime.h:163`), one `BSInputEnableManager` layer, one engine focus-menu name
(`FocusMenu`). SFSE has no shutdown callback (`Plugin.cpp:155-157`), so a mod
that crashes mid-menu must not be able to leave capture/pause stuck on. Mods
supplying intent is safe; mods supplying mechanism is not.

## 2. Two first-class kinds

| | **HUD** (live) | **Menu** (modal) |
|---|---|---|
| Interactive / focusable | no — never becomes the active view | yes — top of stack is active |
| Game state | runs full-speed, keeps all input | input captured; world still simulates (see §3) |
| Multiplicity | many coexist (health + minimap + quest) | one at a time; opt-in nesting |
| Maps onto today | `captureInput=false` passive draw | `captureInput=true` + reconcilers |

**Capture vs. pause are distinct** (this is the key correction from verification):

- **Capture** = the overlay owns input: WndProc freezes gameplay input + routes
  keyboard/mouse into the page, `ControlLayer` disables engine controls
  (**incl. gamepad/XInput — the only gate that catches it**), and `FocusMenu`
  puts the engine in cursor/modal menu-mode. The **world keeps simulating** (NPCs
  move, time passes) — the player just can't act. This is exactly today's
  shipped behavior.
- **Pause** = the simulation itself freezes (engine `IMenu` flag bit27
  `kFlagPausesGame`). This is **RE-unproven for durability** and **deferred to
  Step 3**. The `pausesGame` manifest field is parsed and stored now for a stable
  schema, but is **inert in Steps 1–2**.

**[verified]** `ControlLayer` and `FocusMenu` are both driven by
`DesiredCapture`, **not** `DesiredPause`. Driving control-disable off pause would
let a gamepad drive the game underneath a capturing-but-non-pausing menu
(`ControlLayer.h:11-22` — the WndProc never sees XInput). The shipped `FocusMenu`
creator sets only `ShowCursor|Modal` (0x108), never bit27, so "capture without
pause" is exactly what the engine menu-mode already does.

## 3. Threading contract (load-bearing)

- Bridge commands (`menu.open`, …) run on the **main/game thread** — the renderer
  invokes the web-message handler from `Update()` (`IWebRenderer.h:76-82`), and
  `Update()` runs inside `Tick()` (`Plugin.cpp:26,45`). **[verified]**
- The **F10/Esc path runs on the WndProc (input) thread**
  (`OverlayInputHook` → `Runtime::OnHostKey` → `InputRouter`). `ControlLayer`,
  `FocusMenu`, and `UIMessageQueue` are **main-thread-only**. So the F10/Esc path
  only **enqueues a request**; `Tick()` drains it and mutates the stack on the
  main thread. **[verified: necessary and deadlock-free]**
- The request mutex must stay a **leaf lock**: drain into a local under the lock,
  unlock, then act — never hold it across a renderer / `FocusMenu` / `SetVisible`
  call. **[verified]**
- The renderer ops the applier calls (`SetViewHidden`/`SetViewOrder`/
  `SetActiveView`) are mutex/atomic-guarded and safe from any thread; running them
  from `Tick` is the same context as today's `setViewHidden` bridge command.
  **[verified]**
- `MenuEventSink::ProcessEvent`'s thread is **unproven**, so its force-close is
  routed through the same enqueue path, not called directly. **[verified]**

---

## Step 1 — data model (additive; no behavior change)

Add to `ViewManifest` (`src/runtime/ViewManifest.h`):

```cpp
enum class SurfaceKind : std::uint8_t { Menu, Hud };

struct ViewManifest {
    // ...existing (id, title, entry, width, height, transparent, zorder, interactive, permissions, rootDir)...
    SurfaceKind  kind{ SurfaceKind::Menu };  // "menu" | "hud"
    bool         capturesInput{ true };      // menu-only: freeze + route input while top menu
    bool         pausesGame{ false };        // menu-only: RESERVED for true sim-pause (Step 3); inert now
    bool         openOnStart{ false };       // menu: open at load; hud: show at load
    std::int32_t order{ 0 };                 // within-band z hint (distinct from raw compositing `zorder`)
};
```

**[verified]** `order` is a **dedicated** field, not a reuse of `zorder` —
`zorder` is a live atomic composite sort key mutated by `SetViewOrder`
(`UltralightWebRenderer.cpp:553,1707`). The controller (Step 2) computes the
final band-z (HUD band `[0..999]`, menu band `1000+stackIndex`) and always pushes
it via `SetViewOrder`, so raw `zorder` never governs menu/HUD paint order.

Parse in `ViewManifest::Load` (`src/runtime/ViewManifest.cpp`), defaults
reproducing today; `Json` has no enum helper so `kind` is parsed manually:

```cpp
const auto kindStr = Json::GetString(*json, "kind", "menu");
manifest.kind      = (kindStr == "hud") ? SurfaceKind::Hud : SurfaceKind::Menu;
manifest.capturesInput = Json::GetBool(*json, "capturesInput", true);
manifest.pausesGame    = Json::GetBool(*json, "pausesGame", false);
manifest.openOnStart   = Json::GetBool(*json, "openOnStart", false);
manifest.order = static_cast<std::int32_t>(Json::GetInt(*json, "order", manifest.order));

// HUDs are passive by definition: they draw but never capture, pause, or focus.
// (No per-region hit-testing exists, so an "interactive HUD" isn't expressible.)
if (manifest.kind == SurfaceKind::Hud) {
    if (manifest.capturesInput || manifest.pausesGame) {
        REX::WARN("ViewManifest: HUD '{}' cannot capture input or pause; forcing off", manifest.id);
    }
    manifest.capturesInput = false;
    manifest.pausesGame = false;
    manifest.interactive = false;
}
```

A manifest with none of the new fields → `kind:menu, capturesInput:true,
pausesGame:false` = exactly today's single-overlay behavior. **[verified]** Do not
infer `kind` from the folder name — the shipped `hud` view is actually an
interactive panel (clickable Ping button) and must default to `Menu`.

---

## Step 2 — MenuController + rewiring

### 2a. `MenuController` — pure logic, no engine deps (`src/runtime/MenuController.{h,cpp}`)

Engine-free so it is unit-testable (like `SettingsStore`). Runtime applies the
mechanism. Needs `#include <unordered_set>` (**[verified]** absent from `pch.h`).

```cpp
class MenuController {
public:
    struct Surface { std::string id; SurfaceKind kind; bool capturesInput; bool pausesGame; int order; };
    void Register(const Surface&);                 // idempotent by id
    bool Open(std::string_view id);                // menu: push (single-menu default); hud: show
    bool Close(std::string_view id);               // remove from stack / shown set
    bool CloseTop();                               // pop top menu (Esc / F10-when-open)
    void CloseAll();                               // transitions / panic — clears stack AND hudShown
    bool ToggleDefault(std::string_view defId);    // F10: stack empty ? Open(def) : CloseTop()

    bool DesiredVisible() const;                    // any hud shown || stack non-empty
    std::optional<std::string> ActiveMenu() const;  // stack top (focus target)
    bool DesiredCapture() const;                     // top menu && capturesInput
    bool DesiredPause()   const;                     // top menu && pausesGame  (reserved; unused in Steps 1-2)
    struct Layer { std::string id; bool hidden; int z; };
    std::vector<Layer> DesiredLayers() const;        // HUD band [0..999]=clamp(order); menu band 1000+stackIndex

private:
    std::unordered_map<std::string, Surface> _registry;
    std::vector<std::string>        _menuStack;      // top = back()
    std::unordered_set<std::string> _hudShown;
    bool _singleMenu{ true };                        // Open(menu) auto-closes prior top unless stacking
};
```

**[verified]** `CloseAll()` must clear `_hudShown` **and** `_menuStack`, or a shown
HUD keeps `DesiredVisible` true across a transition.

### 2b. `Runtime::ApplyMenuPolicy()` — the single applier (main thread)

**[verified]** It must own the visibility side-effects itself — `SetVisible`
early-returns on an unchanged value (`Runtime.cpp:260-261`) and init pokes the
compositor directly (`:203-206`), so routing `DesiredVisible=false` through
`SetVisible` at startup would never reach the compositor:

```cpp
void Runtime::ApplyMenuPolicy() {
    if (!_renderer) return;
    for (const auto& L : _menus.DesiredLayers()) {
        _renderer->SetViewHidden(L.id, L.hidden);
        _renderer->SetViewOrder(L.id, L.z);          // derived band-z; raw manifest zorder never governs paint
    }
    if (const auto top = _menus.ActiveMenu()) _renderer->SetActiveView(*top);
    _captureInput.store(_menus.DesiredCapture());     // per-frame policy (the runtime writer that was missing)

    const bool vis = _menus.DesiredVisible();
    const bool wasVisible = _visible.exchange(vis);
    if (_compositor) _compositor->SetVisible(vis);    // unconditional; cheap atomic store
    if (vis && !wasVisible) {                          // cursor recenter on the open edge (as SetVisible does)
        _cursorX = _viewWidth.load() * 0.5f;
        _cursorY = _viewHeight.load() * 0.5f;
        _renderer->InjectMouseMove(int(_cursorX), int(_cursorY));
    }
    // pause (ControlLayer) + engine menu-mode (FocusMenu) are reconciled in Tick off DesiredCapture.
}
```

`IsInputCaptured()` (`Runtime.cpp:326`) is unchanged — it now just reads a
`_captureInput` that tracks the top menu. Zero changes to
`OnHostKey/OnHostChar/OnHostMouse*`.

### 2c. Build the registry + initial state in `Initialize()`

Replace the load/focus loop at `Runtime.cpp:140-165`:

```cpp
for (const auto& id : toLoad) {
    if (const auto* m = _views.Find(id)) {
        _renderer->LoadView(*m);
        _menus.Register({ std::string(id), m->kind, m->capturesInput, m->pausesGame, m->order });
        if (m->openOnStart) _menus.Open(id);
    }
}
if (_config.startVisible && !_menus.DesiredVisible()) _menus.Open(_config.view);  // back-compat
ApplyMenuPolicy();
```

`_config.view` is the **default menu id** (what F10 opens). Drop
`_interactiveViews` / `_activeViewIndex`.

### 2d. Retarget F10/Esc (WndProc thread → enqueue → drain in Tick)

Split `InputRouter::Configure` so the toggle key and Esc invoke **different**
callbacks (today they share one — `InputRouter.cpp:87-94`):

```cpp
void Configure(KeyCode toggleKey, std::function<void()> onToggle, std::function<void()> onBack);
// OnKeyDown: toggle key -> onToggle();  (captured && Esc) -> onBack();
```

Wire in `Initialize()` (replacing `Runtime.cpp:185`) to **enqueue**, not act:

```cpp
_input.Configure(_toggleKey,
    [this]{ EnqueueMenuRequest(MenuReq::ToggleDefault); },
    [this]{ EnqueueMenuRequest(MenuReq::CloseTop); });
```

```cpp
void Runtime::EnqueueMenuRequest(MenuReq r) { std::lock_guard g(_reqMutex); _reqs.push_back(r); }
void Runtime::DrainMenuRequests() {                // first thing in Tick()
    std::vector<MenuReq> reqs;
    { std::lock_guard g(_reqMutex); reqs.swap(_reqs); }
    for (auto r : reqs) {
        if      (r == MenuReq::ToggleDefault) _menus.ToggleDefault(_config.view);
        else if (r == MenuReq::CloseTop)      _menus.CloseTop();
        else if (r == MenuReq::CloseAll)      _menus.CloseAll();
    }
    if (!reqs.empty()) ApplyMenuPolicy();
}
```

### 2e. Bridge commands (main thread — safe to act directly)

In `RegisterPlatformCommands` (`Runtime.cpp:472`), next to `setViewHidden`:

```cpp
a_bridge.RegisterCommand("menu.open",  [this](const json& p, MessageBridge& b){
    std::string id = Json::GetString(p, "view", std::string(b.CurrentSource()));
    if (_menus.Open(id))  ApplyMenuPolicy(); });
a_bridge.RegisterCommand("menu.close", [this](const json& p, MessageBridge& b){
    std::string id = Json::GetString(p, "view", std::string(b.CurrentSource()));
    if (_menus.Close(id)) ApplyMenuPolicy(); });
a_bridge.RegisterCommand("hud.show",   /* _menus.Open(id)  + ApplyMenuPolicy */);
a_bridge.RegisterCommand("hud.hide",   /* _menus.Close(id) + ApplyMenuPolicy */);
```

Repoint the existing `close`/`setVisible` (`Runtime.cpp:476-481`) at
`Close(CurrentSource)` / `Open|Close(CurrentSource)`. **[verified]** No JS change
needed: closing the only open menu empties the stack → `DesiredVisible` false →
overlay dismissed (same as today). If a live HUD is also up, it remains — by
design.

### 2f. Reconcilers read the derived policy

`Runtime.cpp:420-451` — **[verified]** both track `DesiredCapture` (not pause):

```cpp
void Runtime::ReconcileFocusMenu()    { const bool want = _menus.DesiredCapture(); /* engine cursor/modal */ ... }
void Runtime::ReconcileControlLayer() { const bool want = _menus.DesiredCapture(); /* control-disable incl. gamepad */ ... }
```

`DesiredPause` stays wired to nothing until Step 3's true IMenu pause.

### 2g. Transitions, install gate, focus cleanup

- **[verified] `Plugin.cpp:108`**: install `OverlayInputHook` on
  `config.inputSource == "ui"`, **not** `config.captureInput`. The WndProc hook
  drives the toggle key + routing + consumption, so gating install on the static
  capture flag would make F10 inert and prevent runtime capture. `captureInput`
  becomes the default policy, not the install switch.
- **`MenuEventSink.cpp:36-39`**: replace `SetVisible(false)` with
  `EnqueueMenuRequest(MenuReq::CloseAll)`.
- **Retire `CycleActiveView`/`focusKey`** (`Runtime.cpp:404`): focus follows the
  stack top. Remove the `_interactiveViews.size()` reads at `Runtime.cpp:190` and
  `:338` (**[verified]** or the build breaks). Tab no longer cycles between
  simultaneously-composited views — an intended behavior change.

---

## Back-compat

| Today | Under the new model |
|---|---|
| `config.json` `view:"settings"`, `views:["settings","hud"]` | keys unchanged; `view` = F10 default menu, `views` = surfaces auto-registered at load |
| F10 shows settings **and** hud composited, Tab switches focus | **intended change:** F10 opens the default (settings) only; simultaneous composite + Tab-cycle retired |
| [hud/manifest.json](../data/OSFUI/views/hud/manifest.json) (`interactive:true`, clickable Ping) | defaults to `kind:menu`. To showcase a live HUD, re-author it: `kind:hud, openOnStart:true`, drop the button |
| `captureInput`/`focusMenu`/`disableControls` = true | master enables; per-menu `capturesInput` decides actual capture; `pausesGame` inert until Step 3 |
| `startVisible` | opens the default menu at load |

**Behavior change to flag in the changelog:** a visible **HUD alone no longer
disables controls** (the intended win — the game stays playable under a HUD).

---

## Touch-point summary

| File | Change |
|---|---|
| `src/runtime/ViewManifest.h` / `.cpp` | **Step 1** — `SurfaceKind` + 5 fields + parse + HUD guard |
| `src/runtime/MenuController.{h,cpp}` | **new** pure registry/stack/policy logic |
| `src/runtime/Runtime.h` / `.cpp` | `_menus`, request queue, `ApplyMenuPolicy`, `DrainMenuRequests`, `Initialize` loop, bridge cmds, reconciler rewire, drop `_interactiveViews`/`CycleActiveView` |
| `src/input/InputRouter.h` / `.cpp` | `Configure` gains `onBack`; Esc → `onBack` |
| `src/input/MenuEventSink.cpp` | force-close → `EnqueueMenuRequest(CloseAll)` |
| `src/core/Plugin.cpp` | install `OverlayInputHook` on `inputSource=="ui"` |
| manifests / `config.json` | back-compat annotations above |

`xmake.lua` globs `src/**.cpp`, so `MenuController.cpp` is auto-picked up (an IDE
build needs a reconfigure). **[verified]**

## Test plan
- **Unit (MenuController, no engine):** open/close/toggle/close-top/close-all;
  single-menu auto-close; `DesiredVisible/Capture/Pause/ActiveMenu`; layer
  z-bands; HUDs never popped by menu opens.
- **In-game:** F10 opens `settings`, Esc/F10 closes it (parity); a
  `kind:hud, openOnStart:true` view draws during gameplay with the game fully
  live (no control-disable, gamepad works); opening a menu over that HUD captures
  + composites above it + disables gamepad; `LoadingMenu` clears everything; two
  menus with `stacking` nest and Esc pops one at a time.
- **Thread check:** no `ControlLayer`/`FocusMenu`/`UIMessageQueue` call
  originates off the main thread (all via `Tick`).

## Deferred (Step 3+)
- Native `IOSFUISurfaces` API (register/Open/Close from a sibling DLL), on top of
  [`native-plugin-api.md`](native-plugin-api.md)'s bridge.
- `RegisterViewRoot` multi-root hosting (mods ship views from their own folder).
- **True sim-pause** via IMenu `kFlagPausesGame` (bit27) — keep pause on the proven
  `ControlLayer` path; the hardened `IMenu` stays opt-in until validated surviving
  a long in-game session.

## Open decisions for the owner
1. What the shipped `hud` demo becomes (passive HUD vs. second menu). As shipped it
   has no `kind`, so it stays a Menu (interactive, its Ping button works); it is
   registered but only reachable via `menu.open {view:"hud"}` — F10 opens `settings`.
2. Whether to keep any Tab-style focus switch among *stacked* menus (rare) or drop
   it entirely.
3. When to promote `pausesGame` from inert-reserved to the real Step-3 path.
4. **`setViewHidden` overlap (known limitation).** The pre-existing `setViewHidden`
   bridge command / `Runtime::SetViewHidden` directly toggle a view's hidden flag,
   but every loaded view is now a registered surface, so the next `ApplyMenuPolicy`
   (any open/close/transition) recomputes hidden from the MenuController and
   overrides it. Decide whether to deprecate it, route it through `Open`/`Close`, or
   scope it to non-surface views. Left as-is for now (no shipped view depends on it).
