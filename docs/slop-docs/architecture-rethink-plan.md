# OSF UI v2 Architecture Plan

## Purpose

OSF UI v2 is a clean-room rebuild of the runtime that coordinates views inside
Starfield. The goal is a small, understandable core that can grow from proven
use cases without recreating the v1 `Runtime` god object or introducing a module
framework.

The first supported presentation policy is:

- Any number of views may be installed and discovered.
- Zero or one view may be presented in the foreground.
- Opening another view replaces the current foreground view.
- HUDs, layered views, and view stacks are deferred until a concrete use case is
  implemented.

This is a current policy, not a permanent limitation in the underlying model.

## Design decisions

1. **One owner of logical presentation state.** The v2 runtime owns which view
   is requested, opening, or visible. The WebView backend owns browser and GPU
   resources but does not independently decide logical readiness or visibility.
2. **Many definitions, one foreground session.** Discovery and presentation are
   separate. The catalog may contain many views while presentation holds at most
   one active session.
3. **No internal module system.** First-party features are ordinary concrete
   code compiled into OSF UI. There is no `ModuleHost`, feature discovery,
   compile-time feature selection, service locator, or required/optional module
   metadata.
4. **Installed mods are trusted.** v2 does not build a per-view permission or
   security-policy architecture around native bridge access. Low-level IPC
   validation and bounds may remain where they also protect correctness and
   reliability.
5. **Integrations are added only when exercised.** The core does not initially
   know about Papyrus, settings, hotkeys, localization, controllers, recovery,
   HUD ordering, or D3D12.
6. **Existing rendering mechanisms may be reused, not inherited as policy.** The
   WebView2 host, shared-texture transport, compositor, and Scaleform hooks can
   be connected as concrete adapters after the core contract is settled. Their
   existing runtime policy does not define the v2 architecture.

## System boundary

OSF UI remains two cooperating programs:

```text
Starfield process                         WebView2 host process

SFSE lifecycle and requests              WebView2 controllers
            |                                      |
            v                                      v
      v2 Runtime::Tick()  <---- named pipe ----  local web pages
            |
            v
     submitted shared frame
            |
            v
 D3D12 compositor and Scaleform hooks
```

The separation is mechanical:

- The Starfield DLL owns game integration, logical presentation state, input
  policy when it is added, and D3D12 composition.
- The host process owns WebView2 and produces shared GPU frames.
- The Scaleform and D3D12 queue hooks consume already-submitted render state.
  They do not call feature code or mutate the v2 runtime.

## Core model

### View catalog

`ViewCatalog` owns the installed view definitions and resolves a stable string
ID such as `author.mod/inventory`.

Catalog ownership does not imply that a view is instantiated, open, or visible.
The first milestone may retain the existing fixed-depth discovery contract:

```text
views/<modId>/<viewName>/manifest.json
```

Only manifest fields consumed by implemented behavior belong in the v2 contract.
Permissions, HUD policy, startup policy, and other unused fields are not part of
the foundational runtime.

### Foreground session

The absence of an active session represents `Closed`.

```cpp
namespace OSFUI::V2
{
    using SessionId = std::uint64_t;

    enum class ViewPhase
    {
        Opening,
        Visible
    };

    struct ViewSession
    {
        std::string viewId;
        SessionId sessionId;
        ViewPhase phase;
    };
}
```

The runtime owns:

```cpp
std::optional<ViewSession> activeView;
SessionId nextSessionId{ 1 };
```

`SessionId` is the only initial generation value. It identifies one open attempt
and lets the runtime reject a delayed event after a view was closed, replaced,
or reopened. The design does not introduce separate view, document, host, and
presentation token types.

### Transitions

| Input | Current state | Result |
|---|---|---|
| `Open(A)` | Closed | Create a new Opening session for A |
| `Open(A)` | A is Opening or Visible | No change |
| `Open(B)` | A is Opening or Visible | Close A, then create a new Opening session for B |
| `FrameAvailable(session)` | Matching Opening session | Mark the session Visible |
| `Close(A)` | Matching active session | Close it |
| `BackendFailed(session)` | Matching active session | Close it and report the failure |
| Any backend event | Session ID does not match | Ignore it as stale |

Opening B does not create a stack. A is closed before B becomes the foreground
session. Reopening A later creates a new `SessionId`.

## Ownership

### Runtime

The v2 runtime owns:

- The active foreground session.
- Open, close, replacement, and failure transitions.
- Ordered processing of queued requests and backend events.
- The decision to reveal a view after a matching frame is available.

It does not own WebView2 controllers, shared textures, D3D12 objects, bridge
registries, settings schemas, or game input hooks.

### View catalog

The catalog owns parsed view definitions and ID lookup. It does not create
browser documents or track open state.

### WebView backend

The backend owns physical presentation resources:

- Browser-host process and connection.
- WebView document creation and closure.
- Shared frame production.
- Concrete message and input forwarding when those features are added.

It receives a `SessionId` with an open request and returns that same ID with
frame or failure events. It reports facts; it does not own the foreground state
machine.

### Game integration

SFSE callbacks, Papyrus callbacks, menu events, and window input are producers.
They enqueue or latch small requests for the runtime instead of directly
changing presentation state.

Engine-thread-sensitive mutations run from the established main-thread tick.
Passive startup registration does not need a generalized scheduling rule; each
engine call keeps the narrow thread contract proven for that operation.

### Render path

The compositor and hooks remain a separate real-time subsystem. The runtime may
submit or revoke a frame, but the render hook does not call the runtime, bridge,
filesystem, or feature code.

## Current v2 disposition

The existing `src/v2` work is a parts bin, not something that must be either
preserved wholesale or deleted wholesale.

### Keep

- `Core/Version.h`
- `Platform/NativeMainThreadQueue.*`
- The thin menu-event registration adapter
- `ViewManifest`, `ViewDiscovery`, and `ViewCatalog`, after reducing their
  contract to fields used by the current milestone
- Existing host, renderer, compositor, and hook mechanisms outside `src/v2`

### Replace or substantially simplify

- `RuntimeCoordinator`: replace the overlapping sets with one foreground
  session and a small ordered request/event pump.
- `ViewRuntime` and `ViewPresentationController`: replace the menu-plus-HUD
  presentation model with the foreground-session transition model.
- `IViewPresenter` and `WebViewPresenter`: reduce them to a backend boundary
  that owns resources and reports session-tagged facts, not logical policy.
- The combined v2 test executable: split the pure core tests from later adapter
  tests so the core target has no renderer, CommonLib, bridge, or input
  dependencies.

### Park until a concrete milestone needs them

- `BridgeRuntime`
- Papyrus open/close wiring
- `ViewStartupPolicy`
- Input capture and pause policy
- HUD and view-order behavior
- Host recovery
- Settings, hotkeys, localization, diagnostics, and external APIs

Parked code may remain available in Git history. It should not shape or compile
into the clean core merely because it already exists.

## Build boundaries

The clean core test target must use an explicit source list and standard-library
headers. It should not depend on:

- CommonLibSF or Starfield stubs
- The project PCH
- Glaze or nlohmann JSON unless a catalog test specifically requires parsing
- The WebView renderer or D3D12 compositor
- The bridge
- Input hooks

While v2 is being rebuilt, the production target should also use an explicit v2
source list instead of automatically compiling every `src/v2/**.cpp` experiment.
This is development isolation, not a configurable module system.

## Implementation checkpoints

### 1. Foreground core

Implement and test the session transitions without Starfield or WebView2.

Required tests:

- Starts closed.
- Opens A into Opening.
- A becomes Visible only from a matching frame event.
- Closing works from Opening and Visible.
- Opening B replaces A in a defined order.
- Reopening A receives a new session ID.
- Events from old sessions are ignored.
- A matching backend failure closes the session.

Stop after these tests pass. Do not add a production entry point in this
checkpoint.

### 2. Catalog selection

Connect the foreground core to the catalog:

- An unknown ID is refused.
- A known ID supplies the backend descriptor.
- Multiple views may be discovered even though only one can be active.

Keep parsing and filesystem behavior separate from the session transition tests.

### 3. One-view render slice

Connect the existing WebView2 renderer/host and compositor as a backend:

- Open one bundled local page.
- Keep the compositor hidden while the session is Opening.
- Mark the matching session Visible when its first usable frame arrives.
- Close it and prove the compositor is no longer drawing it.
- Replace it with a second discovered view.

This checkpoint needs a fresh in-game test. A unit test or successful build does
not prove the Starfield hook, shared texture, or visible result.

### 4. Player interaction

Add the smallest real open/close entry path and then keyboard and mouse routing.
Only add focus, ControlLayer, cursor, and pause behavior required by the first
interactive menu. Input ownership must be released when the active session
closes or the backend fails.

### 5. First native communication

Add the bridge only when a real page needs a concrete operation. Implement that
operation and the minimum handshake or message routing it requires. Do not port
the complete v1 platform preemptively.

### 6. Later presentation modes

HUDs or layered views begin as a separate milestone with a concrete consumer.
The likely extension is:

```cpp
struct PresentationState
{
    std::optional<ViewSession> foreground;
    std::vector<ViewSession> huds;
};
```

The HUD collection, ordering, input rules, and recovery behavior are not designed
until that milestone. Foreground replacement behavior remains unchanged.

## Non-goals for the foundation

- Runtime-loaded or compile-selectable internal modules
- A generic kernel or capability-port framework
- A per-view security/permission system
- Full v1 bridge, Papyrus, native ABI, settings, or hotkey compatibility
- Multiple foreground views or a view stack
- HUD ordering or persistence
- General host or device-loss recovery
- Refactoring the proven render hook while rebuilding the runtime

## Proof boundaries

- Pure tests prove only state transitions and catalog policy.
- Adapter tests prove only the mocked or headless boundary they exercise.
- A successful build proves compilation and linking.
- Deployment and matching hashes prove which binaries were installed.
- Only a fresh in-game run proves live SFSE lifecycle, thread behavior, WebView2
  frames, Scaleform composition, input, pause, and visible player behavior.

The implementation should advance one checkpoint at a time and keep those proof
categories separate.
