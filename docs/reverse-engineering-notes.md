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

### 1. Per-frame tick (blocks: anything moving)

`Runtime::Tick(dt)` has no caller. Candidates to investigate, in order of
preference (safest first):
- SFSE `TaskInterface` / task queue semantics — does it offer repeated or
  per-frame scheduling, and on which thread?
- CommonLibSF event sources (menu open/close, UI events) that fire regularly.
- An engine main-loop or frame-update function located via RE (last resort,
  needs a proven AddressLib ID and a trampoline hook).

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
