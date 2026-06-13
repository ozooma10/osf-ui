# Reverse Engineering Notes

Tracking every integration point this project needs but does not yet have.
**Policy: none of this may be guessed.** No hardcoded addresses, no inherited
offsets, no "it worked in Skyrim so probably here too". Game addresses/IDs are
only used after being proven against the running target build (game
**1.16.244.0** since the 2026-06-11 patch / SFSE 0.2.21 / AddressLib with
versionlib-1-16-244 — re-verify on any patch).

> **Incident (2026-06-12, proven):** building against a CommonLibSF pinned
> before upstream PR #26 ("fix: correct UI.h layout via BSTSingletonSDM fix")
> shipped a `UI` layout missing the leading virtual `BSTSingletonSDM<UI>`
> base — every base offset short by 0x10. `RegisterSink<MenuOpenCloseEvent>`
> then handed the game a `BSInputEventReceiver` subobject as an event source;
> the game's `RegisterSink` spun/corrupted UI state and the process died on
> save load inside engine sink iteration (Trainwreck: AV at
> `Starfield.exe+02B7320`, no plugin frames — classic latent corruption).
> Countermeasure, now mandatory: `UiInputHook::VerifyUiLayout()` proves the
> live UI object's `BSInputEventReceiver` vptr equals the AddressLib vtable we
> patch before ANY code touches the UI object; on mismatch all UI integration
> is skipped and logged. Keep this guard pattern for every future
> layout-dependent integration.
>
> **Follow-up finding (same day, proven):** the guard then caught a second,
> independent bug — `RE::VTABLE::UI`'s array order does **not** follow base
> declaration order. The `BSInputEventReceiver` vtable is **`VTABLE[10]`**
> (ID 475439), not `VTABLE[0]`/`[1]`. Proof: `tools/parse_versionlib.py`
> resolved all 11 entries from `versionlib-1-16-244-0.bin`; the live in-game
> vptr (`Starfield.exe+0x4d7e408`) matched only entry 10, and that vtable is
> the single 2-slot one in the cluster (next vtable's COL begins 0x18 later,
> vs 0x10 for all the 1-slot ones) — exactly the dtor+PerformInputProcessing
> shape. Same relative layout exists in the 1.16.242 database, so the old
> `VTABLE[0]` assumption was wrong from day one, independent of the patch.
> ⚠ Upstream issue worth filing: consumers reasonably assume VTABLE order
> mirrors base order; for `UI` it doesn't.

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

Verified in-game 2026-06-12: the pump runs at the main menu (`FrameTick:
first per-frame task received` ~7 s after launch, before `kPostDataLoad`).
Answered 2026-06-12 (20:20 run, heartbeat continuity):
- **Loading screens: yes** — heartbeat continued without a gap through both
  `LoadingMenu` open→close cycles (main-menu→save and save→save).
- **Cadence is NOT 1:1 with render framerate.** Observed: ~600 ticks/s at
  the main menu, but ~2,000–2,300 ticks/s during loading screens AND in
  gameplay (HUDMenu up). Either the pump fires more than once per frame or
  the frame loop runs uncapped in those states — do not use tick count as a
  frame count, and self-timed `dt` (already in place) remains mandatory.
- Same-run data: `NullCompositor` logged ~600 frame submissions/s while the
  overlay was visible (tick-driven, consistent with the above).
Still open:
- Does it run while the pause menu is open? (Console menu: yes — observed.)

### 2. D3D12 access — ◐ DEVICE+QUEUE SOLVED (2026-06-12), present timing open

**Device + direct queue: runtime-PROVEN on 1.16.244** (hook-free, in
`OSF RE/` — see its `Investigations/Requests/2026-06-12-d3d12-device-route.md`
and context module `rendering.graphics_core`):

```
root   = *(void**)REL::ID(944397).address()        // g_RendererRoot
device = *(ID3D12Device**)      (*(uintptr_t*)(root + 0x30) + 0x418)
queue  = *(ID3D12CommandQueue**)(*(uintptr_t*)(*(uintptr_t*)(root + 0x28) + 0x08) + 0x60)
```

Proof: both QI cleanly; queue `GetDesc().Type == DIRECT`; queue->GetDevice is
COM-identical to the device; the adapter at root+0x30→+0x410 reports the real
GPU with a LUID matching the device's. Available from `kPostPostDataLoad`.
Phase 2 consumers must re-verify with the same QI checks at startup (cheap)
instead of trusting the offsets blindly.

Still open for Phase 3:
- Present timing: `IDXGISwapChain3::Present` hook vs an engine-level
  "end of frame" function. Must decide hook tech then (minimal, isolated;
  Detours/MinHook only if CommonLibSF trampolines don't suffice). Note
  `OSF RE/src/Probe/RenderPresentProbe.cpp` already captures the live
  swapchain via the create-call hook, but uses a raw pre-1.16 offset —
  re-anchor before reuse. The engine swapchain wrapper (+0x40 =
  IDXGISwapChain3/4*) is mapped in `rendering.graphics_core`.
- Descriptor heap strategy, resource state expectations, HDR/DRS/windowed
  behaviors, and coexistence with Steam overlay/ReShade/RTSS (hook-chain
  friendliness) — all documented in `composite/D3D12Compositor.h`.

### 3. Input event source — ◐ OBSERVATION SOLVED, consumption open (2026-06-12)

**Observation** is implemented in `input/UiInputHook.cpp`: `RE::UI` derives
from `BSInputEventReceiver` (its second base — `BSTSingletonSDM<UI>` is first
and virtual, see the incident note above), whose vfunc
`PerformInputProcessing(const InputEvent*)` receives the per-frame input
queue. We vfunc-swap slot 1 of `RE::UI::VTABLE[10]` (AddressLib ID 475439 —
proven to be the receiver's vtable, see the incident note; the live vptr is
verified to match before the swap), observe `ButtonEvent`s, and always
forward the unmodified queue. **Verified in-game 2026-06-12.**

Key space, proven by observation (not the DIK/InputMap space previously
assumed here): keyboard `idCode` = **Windows VK codes** (F10 → 121 =
`VK_F10`, LAlt → 164 = `VK_LMENU`); mouse `idCode` 0 = left button; mouse
releases carry `heldDownSecs > 0` (only presses start at 0). Keyboard events
arrived on several different thread IDs across the session — do not assume a
single input thread. Gated by config `inputSource` ("none" disables; the
hook is observe-only either way).

Menu lifecycle observation also implemented, hook-free:
`input/MenuEventSink.cpp` registers a `BSTEventSink<MenuOpenCloseEvent>` on
the UI singleton via the documented `RegisterSink` API (kPostPostDataLoad).

Still open:
- In-game verification of both (thread the events arrive on, event ordering,
  whether `heldDownSecs == 0` reliably marks the initial press).
- How to *consume* input (prevent the game acting on it) while the overlay
  has focus — likely menu-mode related; probably requires registering a real
  `IMenu` or manipulating `BSInputEventUser::inputEventHandlingEnabled`-style
  paths. Unknown; do not guess.
- Text input (`CharacterEvent`) and cursor routing for Phase 4.
- Raw Win32 fallbacks (WndProc subclass / Raw Input) remain out of scope:
  they fight the game loop and break controller parity.

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
