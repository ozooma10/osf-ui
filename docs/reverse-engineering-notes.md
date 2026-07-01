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
> Countermeasure, now mandatory: `UiLayoutGuard::VerifyUiLayout()` proves the
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

## Version-specific surface (patch checklist)

Operationalizes the opening policy ("re-verify on any patch"). The compiled
DLL has **one hard pin, and it is data, not code**: the Address Library binary
(`versionlib-1-16-244-0.bin`, user-installed, not vendored) plus the struct
layouts baked into the pinned CommonLibSF commit. Everything game-specific
resolves through those two. Sorted by durability:

**A. ABI/OS-stable — a game patch cannot move these (no re-verify needed):**
- `IDXGISwapChain::Present` **slot 8** and the throwaway-swapchain vtable
  capture (Kiero technique) — DXGI/D3D12 ABI, fixed by Microsoft
  (`composite/D3D12Compositor.cpp`).
- WndProc subclass (`SetWindowLongPtr`) + `EnumWindows` window-find, raw input
  (`WM_INPUT`), VK codes — pure Win32/OS (`input/OverlayInputHook.cpp`,
  `input/InputRouter.cpp`).
- COM QI / DIRECT / same-device guard on the engine pointers — version-
  independent, and the fail-closed check itself (`composite/EngineD3D12.cpp`).
- Ultralight + our own compositor internals (root sig, PSO, shaders).

**B. Resolved through the Address Library (REL::ID → versionlib offset) —
durable across versions *iff* a matching `versionlib-<build>.bin` is installed
AND the pinned CommonLibSF's IDs/layouts cover that build:**
- Device + DIRECT queue: `RE::CreationRendererPrivate::Renderer` (wraps
  `REL::ID(944397)` g_RendererRoot + the renderer offset walk) —
  `composite/EngineD3D12.cpp`. The offset chain lives in CommonLibSF now.
- UI singleton + receiver vtable: `RE::UI::VTABLE[10]` = ID 475439 —
  `input/UiLayoutGuard.cpp`.
- Per-frame tick: `SFSE::TaskInterface::AddPermanentTask` — SFSE owns the
  underlying game hook and maintains it across patches (`core/Plugin.cpp`).
- `RegisterSink<MenuOpenCloseEvent>` — `input/MenuEventSink.cpp`.
- ⚠ The address library fixes **addresses only**. Struct **member offsets**
  (the renderer chain, the UI base layout, every `sizeof`/`offsetof`
  static_assert in CommonLibSF) are hand-RE'd per version and are the real
  fragility — this is exactly what the 2026-06-11/12 incident above proved.

**C. Constants derived by observation against 1.16.244 in *this repo's own*
code (not CommonLibSF's):**
- `kReceiverVtblIndex = 10` (`input/UiLayoutGuard.cpp`) — index into
  `RE::UI::VTABLE`; the array order is NOT base-declaration order. The one bare
  game-derived integer we carry. Guarded by `VerifyUiLayout()`, which dumps all
  entries on mismatch so it can be re-derived (`tools/parse_versionlib.py`).

**Run on every game / SFSE / CommonLibSF bump:**
- [ ] Install/confirm `versionlib-<new build>.bin` (Address Library) for the
      running build.
- [ ] Bump SFSE if required; confirm runtime + `XSE_SF_*` env.
- [ ] Pull CommonLibSF to a commit covering the build; rebuild. (Pinned
      submodule currently trails: `RUNTIME_LATEST` = 1.16.236,
      `SFSE_PACK_LATEST` = 0.2.19, vs target 1.16.244 / SFSE 0.2.21.)
- [ ] Launch; confirm `VerifyUiLayout()` passes. On mismatch it logs every
      `UI::VTABLE[i]` — re-derive `kReceiverVtblIndex` from the entry flagged
      `<-- matches live vptr`.
- [ ] Confirm `EngineD3D12` logs a QI-verified device + DIRECT queue (a layout
      drift fails this guard instead of crashing).
- [ ] Confirm the Present hook still catches presents (both swapchains) and the
      overlay draws.
- [ ] Confirm input capture freezes gameplay and the toggle releases.

**Stale-but-harmless declaration:** the generated `SFSEPlugin_Version` sets
`CompatibleVersions({ RUNTIME_LATEST })` = **1.16.236** in the pinned submodule,
not the 1.16.244 target. It loads anyway because `UsesAddressLibrary(true)`
makes SFSE ignore the list. If the address-library flag is ever turned off
(xmake `commonlibsf.plugin` options), this mismatch will block loading.

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

**Present timing: DECIDED 2026-06-12 — hook `IDXGISwapChain::Present`
(vtable slot 8).** Runtime-proven in `OSF RE` (RenderPresentProbe,
re-anchored to ID 141996; see its
`Investigations/Requests/2026-06-12-present-timing.md` and the present note in
`rendering.graphics_core`):

- A single vfunc hook on slot 8 caught presents from BOTH live swapchains
  (they share one vtable) — one hook covers everything.
- Slot 8 = Present is DXGI-stable ABI, so the *hook point* needs no
  per-patch AddrLib anchoring (unlike an engine fn). The engine's own
  present (`EngineSwapChainPresent` @ RVA 0x2A0EECB) does
  `call [pDxSwapChain_vtable + 0x40]` = slot 8, with syncInterval/flags from
  `GameSwapChainWrapper+0x50/+0x54` — corroboration, not the hook site.
- Present fires on a single thread, `GetDesc()` works in the thunk,
  BufferCount=2, syncInterval=1.

Phase 3 caveats from the same run:
- **Two swapchains present every frame** (same HWND): one straight from the
  engine, one from an injected high module (a frame-gen/upscaler/Reflex
  interposer — DLSS-G/Streamline/AGS class). Draw into the one actually
  scanned out, or hook both. Don't assume a single swapchain.
- Remaining Phase 3 work (unchanged): record our own command list off the
  located device/queue, transition the overlay texture to
  PIXEL_SHADER_RESOURCE, draw a fullscreen alpha-blended quad onto the
  current backbuffer RTV (`GetBuffer(GetCurrentBackBufferIndex())`),
  transition back, then call original Present. Own descriptor heaps + root
  signature + PSO; handle HDR/DRS/windowed; test coexistence with Steam
  overlay/ReShade/RTSS (hook-chain ordering). Hook tech: CommonLibSF vtable
  hook or MinHook, minimal + isolated.
- Descriptor heap strategy, resource state expectations, HDR/DRS/windowed
  behaviors, and coexistence with Steam overlay/ReShade/RTSS (hook-chain
  friendliness) — all documented in `composite/D3D12Compositor.h`.

### 3. Input — ✅ OBSERVATION + CONSUMPTION + KEYBOARD SOLVED (2026-06-12)

**Consumption (the hard part) is solved at the WndProc, NOT the engine input
sink.** Proven in-game 2026-06-12: the UI input sink
(`UI::PerformInputProcessing`) is only ONE of several sinks on the shared
input queue; gameplay movement and camera/mouse-look read the same input
through sibling paths (OSF RE `platform.input_windowing`: the translated-input
fanout hits `BSInputDeviceManager`, `MenuControls`, `MenuCursor`, `UI`, …).
So **neither** passing the UI sink an empty queue **nor** marking every event
consumed (`InputEvent::status = kStop` + `IDEvent::disabled`) stops the
player — both were tried and both failed. The working mechanism is
`input/OverlayInputHook.cpp`: a **WndProc subclass** on the game's own window
(`SetWindowLongPtr(GWLP_WNDPROC)` — a window subclass, NOT a global
`SetWindowsHookEx`). While `Runtime::IsInputCaptured()`, it consumes
`WM_INPUT` (raw mouse-look + keyboard), `WM_KEY*`, `WM_CHAR`, and all mouse
messages, freezing the game; the toggle key is always consumed so it never
leaks. Keyboard is routed from the WndProc (VK → `InputRouter` →
`UltralightWebRenderer::InjectKeyEvent` → `View::FireKeyEvent`). **Verified
in-game 2026-06-12: movement + camera fully frozen with the overlay open,
typing lands in the page, F10 releases.** (Earlier worry that a WndProc
subclass "fights the game loop / breaks controller parity" did not bear out
for keyboard+mouse; gamepad parity is a later concern.)

**Observation** was implemented as an observe-only vfunc swap on
`UI::PerformInputProcessing` (slot 1 of `RE::UI::VTABLE[10]`, AddressLib ID
475439 — proven to be the receiver's vtable): `RE::UI` derives from
`BSInputEventReceiver` (its second base — `BSTSingletonSDM<UI>` is first and
virtual, see the incident note above), whose vfunc receives the per-frame
input queue. Verified in-game 2026-06-12; this is where the VK key space and
mouse idCodes below were proven. **Removed 2026-07-01**: it was
diagnostic-only (routing/consumption live at the WndProc), so the hook came
out — one less engine vtable write. What survives is the layout guard,
`UiLayoutGuard::VerifyUiLayout()` (`input/UiLayoutGuard.cpp`), which still
gates every UI integration at kPostPostDataLoad.

Key space, proven by observation (not the DIK/InputMap space previously
assumed here): keyboard `idCode` = **Windows VK codes** (F10 → 121 =
`VK_F10`, LAlt → 164 = `VK_LMENU`); mouse `idCode` 0 = left button; mouse
releases carry `heldDownSecs > 0` (only presses start at 0). Keyboard events
arrived on several different thread IDs across the session — do not assume a
single input thread. (These findings were proven through the since-removed
observe hook; they remain the reference for the key space the WndProc path
consumes.)

Menu lifecycle observation also implemented, hook-free:
`input/MenuEventSink.cpp` registers a `BSTEventSink<MenuOpenCloseEvent>` on
the UI singleton via the documented `RegisterSink` API (kPostPostDataLoad).

Still open (Phase 4b and later):
- **Mouse + cursor.** The OS cursor is hidden/clipped during gameplay, so
  read `WM_INPUT` raw mouse deltas in the WndProc, accumulate a virtual
  cursor in view space, route MouseMoved/Down/Up into the view, and draw a
  visible pointer. This makes the page buttons clickable. (We chose the
  WndProc + raw-delta route because `MouseMoveEvent`'s layout isn't in
  CommonLibSF and must not be guessed.)
- Unicode/IME text via the `WM_CHAR` path (current keyboard routing
  translates VK→char US-layout-only).
- Gamepad routing + controller parity once a focus model for pads exists.
- ESC/pause nuance, save/load force-hide.

### 4. Menu/pause/UI lifecycle questions

> **Filed as OSF RE requests (2026-06-13) — RESOLVED.** The "feels janky when
> menus are open" fix is to register a real `IMenu` so the engine enters menu
> mode (it stops feeding gameplay input, shows a cursor, optionally pauses)
> instead of the WndProc message-swallow, which the world keeps running behind
> and which gamepad/XInput leaks past. Three probes ran:
> `OSF RE/Investigations/Requests/2026-06-13-custom-imenu-registration.md`
> (custom-menu register + open PROVEN; headless IMenu crash root-caused → ship a
> hardened engine-built creator), `.../2026-06-13-imenu-flag-bits.md`
> (`RE::IMenu::Flag` bits proven: bit3 ShowCursor, bit8 kModal, bit27
> kPausesGame), and `.../2026-06-13-input-enable-layer-control-disable.md`
> (device-agnostic control disable via `BSInputEnableManager`/`BSInputEnableLayer`
> PROVEN live with a controller — keyboard + mouse-look + gamepad all freeze; flag
> table mapped, `Looking`=camera). Both land in OSF UI behind config
> `focusMenu` / `disableControls` (src/input/FocusMenu.{h,cpp},
> src/input/ControlLayer.{h,cpp}); the shipped config.json now enables both. The
> one remaining live unknown is whether the hardened focus menu survives past the
> few-second mark the headless one crashed at.

- Is there a "menu open" state that pauses simulation, and can a plugin open
  a custom one (SFSE `MenuInterface` — what does it actually expose)?
- How does the game arbitrate cursor visibility, and what happens to mouse
  capture when an overlay wants the cursor?
- Save/load and main-menu transitions: when must the overlay force-hide?

## Workspace note

RE work for this workspace happens in `OSF RE/` per its rules (proof
requirements for offsets/IDs). Findings that OSF UI consumes should be
recorded there and referenced here — this file tracks *questions*, not
unproven answers.
