# Reverse Engineering Notes

Tracking every integration point this project needs but does not yet have.
**Policy: none of this may be guessed.** No hardcoded addresses, no inherited
offsets, no "it worked in Skyrim so probably here too". Game addresses/IDs are
only used after being proven against the running target build (game
1.16.242.0 / SFSE Address Library v21 at the time of writing — re-verify on
any patch).

## Unknowns that must not be guessed

- Starfield renderer internals: device/queue ownership, frame graph, where UI
  is composited (the engine's own UI uses a scaleform-successor stack whose
  draw point is unknown to this project).
- Any vtable layout, menu class, or singleton address not already proven by
  CommonLibSF itself.
- Thread affinity rules: which threads own D3D12 submission, input, and
  script VM. Do not call game systems from a private thread.

## Needed integration points

### 1. Per-frame tick — ✅ SOLVED (SFSE TaskInterface, 2026-06-12)

`Runtime::Tick(dt)` is driven by `SFSE::TaskInterface::AddPermanentTask`
(registered in `core/Plugin.cpp`). Evidence, from SFSE source (not guessed):

- `sfse/PluginAPI.h` documents `AddTaskPermanent` as "executed every frame on
  the Main thread without deleting".
- `sfse/Hooks_Command.cpp` shows the pump: SFSE hooks a per-frame function
  (`Command_Process`) and runs all permanent tasks, then one-shot tasks,
  under a recursive mutex. The hook offset belongs to SFSE and is maintained
  by SFSE across game patches.

Constraints derived from that implementation:
- No `RemovePermanentTask` exists → our delegate has process lifetime and a
  no-op `Destroy()`.
- `Run()` executes under SFSE's task-queue lock → must stay cheap, never
  block, never wait on other threads.
- No dt is provided → we self-time with `steady_clock`, clamped to 100 ms
  (the game pauses on focus loss; the task stalls with it).

Still unverified in-game (heartbeat logging is in place to answer these):
- Does the pump run at the main menu? While the pause menu is open? In
  loading screens?
- Actual cadence vs render framerate.

### 2. D3D12 access (blocks: Phase 2/3)

- `ID3D12Device` + direct `ID3D12CommandQueue` retrieval: engine renderer
  singleton (find + prove), or vtable-hook of factory-created objects at init.
- Present timing: `IDXGISwapChain3::Present` hook vs an engine-level
  "end of frame" function. Must decide hook tech then (minimal, isolated;
  Detours/MinHook only if CommonLibSF trampolines don't suffice).
- Descriptor heap strategy, resource state expectations, HDR/DRS/windowed
  behaviors, and coexistence with Steam overlay/ReShade/RTSS (hook-chain
  friendliness) — all documented in `composite/D3D12Compositor.h`.

### 3. Input event source (blocks: Phase 4)

- Where PC keyboard/mouse events can be observed (BSInputDeviceManager
  equivalent? PlayerControls? SFSE input interface?). CommonLibSF ships
  `SFSE::InputMap` — check what it maps and whether an event sink exists.
- How to *consume* input (prevent the game acting on it) while the overlay
  has focus — likely menu-mode related, unknown.
- Raw Win32 fallbacks (WndProc subclass / Raw Input) are explicitly out of
  scope for now: they fight the game loop and break controller parity.

### 4. Menu/pause/UI lifecycle questions

- Is there a "menu open" state that pauses simulation, and can a plugin open
  a custom one (SFSE `MenuInterface` — what does it actually expose)?
- How does the game arbitrate cursor visibility, and what happens to mouse
  capture when an overlay wants the cursor?
- Save/load and main-menu transitions: when must the overlay force-hide?

## Workspace note

RE work for this workspace happens in `OSF RE/` per its rules (proof
requirements for offsets/IDs). Findings that StarfieldWebUI consumes should be
recorded there and referenced here — this file tracks *questions*, not
unproven answers.
