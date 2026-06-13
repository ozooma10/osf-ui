# StarfieldWebUI — Resume / Handoff

**Last updated:** 2026-06-13 ~05:15
**Status:** 🎉 **END-TO-END WORKING + FULLY INTERACTIVE.** Phase 0 + TODOs
#1–#5 + renderer-plan **Phases 1, 2, 3, 4a AND 4b** all done/verified. Pixels
over Starfield: WebKit (Ultralight 1.4.0) renders offscreen → GPU texture on
the game's device → alpha-blended quad in an `IDXGISwapChain::Present` slot-8
hook → **visible on screen**. AND fully interactive: with the overlay open the
game is **input-frozen** (movement + camera), **typing lands in the page**,
and the **mouse drives a software cursor that clicks the buttons** (Ping
round-trips to a pong) — all via a WndProc subclass (the engine UI sink can't
block gameplay; RE notes §3). F10 toggles + releases. The swapchain-resize
crash is **fixed** (compositor no longer caches backbuffer refs — confirm
once more with a live resolution change). Next: Phase 3 polish + 4c. See
§0/§7.
Game is **1.16.244.0** (patched 2026-06-11; SFSE 0.2.21, versionlib-1-16-244
present in the AIO address library mod).

---

## 0. IMMEDIATE next step

Phases 1–3 + 4a + 4b are all done/user-verified: the overlay renders over the
game AND is fully interactive (keyboard + mouse + clickable buttons + input
freeze). Highest-value next items:

- **✅ Compositor swapchain-resize crash — FIXED 2026-06-13.** The compositor
  was caching the swapchain's backbuffer references across frames, blocking
  the game's `ResizeBuffers` → AV after a resolution change (2026-06-12
  23:18). Now it does `GetBuffer` + `CreateRTV` per-present and releases the
  backbuffer immediately (`RecordAndExecute`), so it never holds a ref across
  frames. No rendering regression (verified 05:18). Worth one more live
  resolution-change to confirm the crash is gone.
- **Phase 3 polish** (renderer-plan.md): ✅ aspect-correct view sizing done
  (view tracks the output surface; no more stretch — 2026-06-13). Remaining:
  HDR/10-bit backbuffer PSO + multi-format PSO, pick the scanned-out
  swapchain under frame-gen, Steam/ReShade/RTSS coexistence, sRGB pass,
  dirty-region upload.
- **Phase 4c** (renderer-plan.md): scroll, IME/Unicode text, gamepad,
  cursor sensitivity + aspect-correct mapping.
- **Phase 5**: MCM-style schema-driven UI.

Compositor entry points if you reopen it: `composite/D3D12Compositor.cpp`
(`OnPresent` = the per-frame draw; `RecordAndExecute` = the command list;
`InstallPresentHook` = the dummy-swapchain vtable capture).

Quick re-verify of the Ultralight backend (one launch to the main menu, no
interaction): expect in `StarfieldWebUI.log` —
`SDK DLLs preloaded` → `worker calling Renderer::Create()` →
`first paint (1280x720…)` → `DOM ready` →
`MessageBridge: [web] test view loaded; bridge online` →
`post-DOM frame dumped to …first-frame.png` (PNG shows the test panel with
"Connected: StarfieldWebUI v0.1.0").

This file is the single place to re-orient after switching machines. Read it,
then read [architecture.md](architecture.md) and
[reverse-engineering-notes.md](reverse-engineering-notes.md).

---

## 1. What this project is

An SFSE/CommonLibSF plugin (`StarfieldWebUI`) that will eventually host
HTML/CSS/JS UI views inside Starfield (Prisma-UI-*inspired*, no Prisma code).
Built from [libxse/commonlibsf-template](https://github.com/libxse/commonlibsf-template),
C++23 + XMake, GPL-3.0.

Repo root: `C:\Modding\Starfield\OSF UI`
(renamed from `StarfieldWebUI` per the workspace naming convention settled
2026-06-12: repo folder == xmake target == MO2 mod folder. The plugin
*metadata* name stays `StarfieldWebUI` — it drives the SFSE log filename and
the `SFSE/Plugins/StarfieldWebUI/` data folder. Part of the larger
multi-repo Starfield modding workspace.)

---

## 2. How to pick up on the new machine

### Prerequisites
- XMake 3.0.0+
- MSVC or Clang-CL with C++23
- (Optional, Phase 1 only) an Ultralight SDK — **never vendored**; set
  `ULTRALIGHT_SDK_DIR` if you build with it.

### Clone (recursive — submodules are CommonLibSF + commonlib-shared)
```bat
git clone --recurse-submodules <your-remote> StarfieldWebUI
```
> `origin` = `https://github.com/ozooma10/osf-ui.git` (correct). ⚠ As of this
> writing all of today's fixes are **uncommitted** (10 modified files +
> untracked `tools/`) — commit + push before switching, or copy the working
> tree. See §6 for the exact list.

### Build
```bat
cd StarfieldWebUI
xmake build
```
- Cold build compiles CommonLibSF too (~13 s). Incremental is ~1–2 s.
- If `XSE_SF_MODS_PATH` is set, output auto-installs to
  `<mods>\StarfieldWebUI\SFSE\Plugins\...` (on the current machine that is
  `C:\Modding\Starfield\MO2\mods\OSF UI`). On the new machine set
  `XSE_SF_MODS_PATH` or `XSE_SF_GAME_PATH` to get auto-deploy.

### Ultralight build (the real backend — this is now the dev default)
```bat
set ULTRALIGHT_SDK_DIR=C:\Modding\Starfield\OSF UI\external\ultralight-free-sdk-1.4.0-win-x64
xmake f --with_ultralight=true
xmake build
```
- The SDK lives in `external\` (gitignored — **never vendored**; free 1.4.0
  win-x64 drop). Fails with a clear message if the SDK dir is missing.
- The install step also ships the runtime payload into the mod folder:
  `SFSE/Plugins/StarfieldWebUI/ultralight/bin/*.dll` (the four SDK DLLs) and
  `ultralight/resources/icudt67l.dat` (ICU). `cacert.pem` is deliberately
  NOT shipped (network stays off — security-model.md §2).
- Shipped `config.json` now says `renderer=ultralight`. A default
  (Ultralight-free) build with that config falls back to the null renderer
  with a clear warning — the **default build must still compile and run
  Ultralight-free**.

---

## 3. Current state — what works

| Area | State |
|---|---|
| SFSE preload/load lifecycle + logging | ✅ implemented |
| Paths (resolved relative to the DLL, MO2-VFS safe) | ✅ |
| Config load (`config.json`, defensive, defaults on missing) | ✅ |
| View manifest discovery + validation | ✅ |
| Renderer abstraction: Null, Mock (CPU RGBA test pattern), **Ultralight (real, offscreen)** | ✅ |
| **TODO #3** — Ultralight backend (worker thread, CPU surface, sandbox FS, JS bridge) | ✅ implemented, ✅ **verified in-game 2026-06-12 20:58** (PNG dump + bridge round-trip) |
| Compositor abstraction: Null, **D3D12 (present-time overlay — Phase 3)** | ✅ implemented, ✅ **drawn over the game & user-verified 2026-06-12 22:09** |
| JSON message bridge (whitelist: close/log/ping/setVisible) | ✅ |
| Sample `test` view (HTML/CSS/JS, runs standalone in a browser too) | ✅ |
| **TODO #1** — per-frame `Runtime::Tick()` via SFSE `TaskInterface` | ✅ implemented, ✅ **verified in-game 2026-06-12** |
| **TODO #2** — input observation (UiInputHook) + menu events (MenuEventSink) | ✅ implemented, ✅ **fully verified in-game 2026-06-12** (menu events, key/mouse observation incl. mouse release, layout guard, VK key space, F10 toggle → frames — §0) |
| Docs (architecture / renderer-plan / security-model / RE-notes) | ✅ |

---

## 4. Key implementation facts (so you don't re-derive them)

### Tick source (TODO #1 — done)
- `Runtime::Tick(dt)` is driven by `SFSE::TaskInterface::AddPermanentTask`,
  registered in [Plugin.cpp](../src/core/Plugin.cpp) (`FrameTickTask`).
- Evidence (not guessed): `sfse/PluginAPI.h` documents permanent tasks as
  "executed every frame on the Main thread"; `sfse/Hooks_Command.cpp` shows
  SFSE pumping them from a per-frame hook under a recursive mutex.
- Constraints baked in: process-lifetime static delegate, no-op `Destroy()`
  (there is no Remove API), self-timed `dt` clamped to 100 ms (game pauses on
  focus loss). `Run()` must stay cheap (runs under SFSE's lock).
- Heartbeat logging: INFO on first tick, DEBUG every 600 ticks.

### 4a. The 2026-06-12 save-load crash (root-caused and fixed)

First in-game run crashed on save load (Trainwreck: AV at
`Starfield.exe+02B7320`, engine sink-iteration code, `UI*` in registers, no
plugin frames). Cause: `lib/commonlibsf` was pinned to 2026-06-05, **before
upstream PR #26** ("fix: correct UI.h layout via BSTSingletonSDM fix",
merged 2026-06-10 — authored by this repo's owner). The real `UI` object
starts with a virtual `BSTSingletonSDM<UI>` (0x10 bytes incl. vptr), so the
old layout had every base short by 0x10:

- `RegisterSink<MenuOpenCloseEvent>` passed `ui+0x10` (actually
  `BSInputEventReceiver`) to the game's `RegisterSink` → it never returned
  (the missing "MenuEventSink: registered" log line is the tell — the logger
  flushes every INFO) and corrupted UI state → AV ~2 min later on save load.
- `UiInputHook` had the twin bug: `VTABLE[0]` is the SDM vtable, not
  `BSInputEventReceiver`'s.

Fixes (all in working tree as of this writing):
- submodule updated to upstream `4c48ed4` (includes PR #26–28),
- **`UiInputHook::VerifyUiLayout()`**: live-vptr-vs-AddressLib guard that
  must pass before MenuEventSink or UiInputHook touch the UI object; on
  mismatch everything UI-related is skipped with a loud ERROR (and all 11
  vtable entries are dumped for re-derivation). This is the pattern for all
  future layout-dependent integrations.
- hook retargeted to `VTABLE[10]` slot 1. The first re-test run had the guard
  (correctly) refuse `VTABLE[1]`: the IDs_VTABLE array order does NOT follow
  base order. `tools/parse_versionlib.py` + the live vptr proved entry 10
  (ID 475439) is the receiver's vtable — full story in
  [reverse-engineering-notes.md](reverse-engineering-notes.md).
- `main.cpp` passes `logLevel = Debug` to `SFSE::Init` so DEBUG lines
  (tick heartbeat, menu events) are on disk when the game dies.

### Input (TODO #2 — done)
- **Key space (CORRECTED 2026-06-12, in-game proof):** Starfield keyboard
  `ButtonEvent::idCode` carries **Windows VK codes** — F10 arrived as 121
  (`VK_F10`), left Alt as 164 (`VK_LMENU`). The previous claim here ("DIK
  scan codes / InputMap space, NOT VK") had it exactly backwards; key names
  now resolve to VK in `ResolveKeyName`. Mouse `idCode` observed: 0 = LMB.
  Mouse releases always have `heldDownSecs > 0` — never filter them with the
  initial-press check (that bug ate every mouse-up in the first test).
- [UiInputHook.cpp](../src/input/UiInputHook.cpp): the project's ONLY hook.
  A vfunc swap on `RE::UI::VTABLE[10]` slot 1 (`PerformInputProcessing`;
  the IDs_VTABLE array is NOT in base order — entry 10 / ID 475439 is the
  proven receiver vtable), gated by the `VerifyUiLayout()` live-vptr guard. **Observe-only**: reads button
  events, feeds `InputRouter`, always forwards the unmodified queue. Gated by
  config `inputSource` (`"none"` = off in code; shipped config uses `"ui"`).
  Install is **one-way** (no safe un-swap once other overlays chain on) —
  disabling uses a pass-through flag, not an unhook.
- [MenuEventSink.cpp](../src/input/MenuEventSink.cpp): hook-free
  `BSTEventSink<MenuOpenCloseEvent>` registered on the UI singleton via the
  documented `RegisterSink` API at `kPostPostDataLoad`.
- Toggle path is wired end to end: `F10` (VK 0x79) → router →
  `Runtime::ToggleVisible()` → mock frames to the null compositor. Every
  link except the final toggle→frame step is verified; that's §0.

### Ultralight backend (TODO #3 — done; hard-won facts)
- **Threading:** ALL Ultralight/WebCore calls live on one dedicated worker
  thread owned by `UltralightWebRenderer` (WebKit is thread-affine; SFSE
  ticks arrive on varying OS threads). Game-thread API only touches mutexed
  queues + a double-buffered BGRA frame copy. The worker self-paces ~60 Hz.
- **SFSE's plugin-load phase is pre-main and fragile** (process has ~3
  threads, usvfs hooks mid-bootstrap). Two hangs were root-caused there:
  1. The SDK DLLs are delay-loaded, and merely CONSTRUCTING the renderer
     fires delay-load resolution (MSVC imports compiler-generated special
     members of dllimport classes — the Impl's SDK base-class ctors).
     `UltralightWebRenderer::PreloadRuntime()` must load the DLLs from
     `<data>/ultralight/bin` (dependency order: UltralightCore, WebCore,
     Ultralight, AppCore) BEFORE the object is constructed
     (`Runtime::CreateRenderer` does this).
  2. Heavy init (Renderer::Create / WebCore thread pool) deadlocks in that
     phase → the worker thread starts lazily on the FIRST tick.
- **xmake gotcha:** this target is a shared lib → linker flags go in
  `shflags`, NOT `ldflags` (the /DELAYLOAD flags were silently dropped at
  first, producing static imports and SFSE's "couldn't load plugin
  (0000007E)" dialog). Verify with `dumpbin /DEPENDENTS`: the four SDK DLLs
  must be under "delay load dependencies".
- **Symbol homes (1.4.0):** C++ core API (Platform/String/Buffer/
  BitmapSurface) = UltralightCore.lib; JavaScriptCore C API = WebCore.lib;
  Renderer/View = Ultralight.lib; `GetPlatformFontLoader` = AppCore.lib
  (the ONLY AppCore use — no window/app machinery).
- **JS bridge:** `postMessage` injected at `OnWindowObjectReady` (JSC C API,
  read-only property); native→web queues until `OnDOMReady` then calls
  `window.starfield.onMessage(json)`. Both queues capped at 64. The test
  view auto-sends `log` + `ping` on load so the round trip needs no input.
- The first paint fires BEFORE DOM ready (blank white) — the devMode PNG
  dump waits for the first post-DOM paint.
- `IWebRenderer` grew `SetWebMessageHandler()` (web→native delivery happens
  on the game thread inside `Update()`) and `RendererConfig.dataDir`.

### D3D12 compositor (Phases 2 + 3 — done; key facts)
- `composite/EngineD3D12.cpp` is the ONLY consumer of the proven device
  route (REL::ID(944397) + offsets from `rendering.graphics_core`). It
  re-proves the route on every locate: guarded reads, QI on device AND
  queue, queue type must be DIRECT, queue->GetDevice must be COM-identical
  to the device. Any failure → null result → overlay disabled loudly.
- **Threading is the whole design.** `Submit` (SFSE tick thread) only
  memcpys the CPU frame into a lock-guarded staging buffer (deduped by
  `frameIndex` — the renderer returns its cached frame between repaints).
  ALL GPU work happens on the **render thread** inside the Present hook
  (`OnPresent`): upload→texture, then draw. No cross-thread resource state.
- **Present hook = the Kiero trick:** create a 1×1 throwaway swapchain on
  the located queue, read its vtable slot 8 (Present), VirtualProtect-swap
  it for our thunk, release the dummy. The vtable is shared by every
  swapchain in the process, so the game's real presents hit our thunk. Zero
  engine offsets — only `g_originalPresent`/`g_overlay` process-statics so
  the thunk survives Impl teardown (we never unhook — one-way, like the
  input hook).
- Per present: optional upload (if a new frame arrived); texture
  COPY_DEST→PIXEL_SHADER_RESOURCE; backbuffer PRESENT→RENDER_TARGET; set
  heaps/rootsig/PSO/viewport/RTV; `DrawInstanced(3)` (fullscreen triangle
  from SV_VertexID, no vertex buffer); backbuffer RT→PRESENT; texture
  PSR→COPY_DEST; execute on the located queue; signal the slot fence. Busy
  ring (3 slots) SKIPS the draw rather than blocking the render thread.
- Shaders compiled at runtime via `D3DCompile` (game already loads
  D3DCompiler_47.dll). Blend is premultiplied-over (ONE / INV_SRC_ALPHA) —
  Ultralight's BGRA surface is premultiplied. SRV format = BGRA8 so the PS
  samples correct RGBA without a manual swizzle even onto an RGBA8 RT.
- PSO is built for the live backbuffer RT format (first one seen). A second
  swapchain with a different format is skipped + warned (single-format for
  now — HDR is Phase 3 polish). Backbuffer RTVs cached per swapchain, keyed
  by pointer, rebuilt on resize.
- Visibility: `ICompositor::SetVisible` (Runtime pushes initial
  `startVisible` + every toggle). Present hook draws only when visible —
  without it the hook would keep redrawing the last frame after hide.

### Hard rules being honored
- No invented addresses/offsets/vtables/menu names. The single vtable ID comes
  from CommonLibSF, not from us.
- D3D12 compositor now **draws the overlay over the game** at present time
  (Phase 3, user-verified). Shipped config: `compositor=d3d12`,
  `startVisible=true` (overlay shows from launch; F10 toggles).
- JS is untrusted: defensive JSON parse, command whitelist, no arbitrary
  native calls, network/filesystem forced off.

---

## 5. First in-game smoke test (do this when you have game access)

1. Enable `StarfieldWebUI` in MO2, launch via SFSE.
2. Open the SFSE log: `Documents\My Games\Starfield\SFSE\Logs\StarfieldWebUI.log`
   (resolve Documents via OneDrive redirection — don't hardcode).
3. Confirm, in order:
   - `preload entered` / `load entered`
   - `Config: loaded ... (inputSource=ui ...)`
   - `per-frame tick registered via SFSE TaskInterface`
   - `FrameTick: first per-frame task received` (proves #1 — ✅ seen 2026-06-12)
   - **no `UI layout guard FAILED` ERROR** (if present: CommonLibSF layout vs
     game version mismatch — STOP, do not play, fix the lib first)
   - `MenuEventSink: registered` and `UiInputHook: installed` (proves #2 wired)
4. Load a save (this is what crashed pre-fix), then press **F10** → expect
   `Runtime: overlay visibility -> true` and
   `NullCompositor: first frame submitted`.
5. Answered 2026-06-12 (recorded in
   [reverse-engineering-notes.md](reverse-engineering-notes.md)): tick pumps
   at the main menu; input arrives on **multiple thread IDs** (no single
   input thread); `heldDownSecs == 0` reliably marks initial key presses
   (but mouse releases always have it > 0); menu names log exactly as
   expected (`MainMenu`, `FaderMenu`, `LoadingMenu`, `HUDMenu`,
   `HUDMessagesMenu`, `CursorMenu`; 20:20 run added `Console`). Loading
   screens answered 20:20: tick pumps straight through them, and cadence is
   NOT 1:1 with render frames (~600 ticks/s main menu vs ~2,000+/s
   loading/gameplay — details in the RE notes). Still open: tick during the
   pause menu.

---

## 6. ⚠ Before/after the transfer — git hygiene

- `origin` = `https://github.com/ozooma10/osf-ui.git` (already correct).
- ✅ **Resolved 2026-06-12 evening:** all of the crash fix + verification
  work listed here previously is committed as `682cfa7` ("UI Input hooks"),
  with the submodule pinned at `4c48ed4`.
- Still uncommitted (rename follow-up only): `xmake.lua` (target
  `StarfieldWebUI` → `OSF UI`), `README.md`, `docs/HANDOFF.md` (deploy-path
  and status updates). Safe to commit as e.g. "Rename target to OSF UI per
  workspace naming convention".
- Do not run `git checkout --`/reset/stash on anything you didn't dirty —
  several workspace repos are intentionally dirty.
- `build/` and any `vsxmakeXXXX/` dirs are regenerable; no need to copy them.
- The deployed DLL in `MO2\mods\OSF UI` (`OSF UI.dll`, built 2026-06-12
  19:52) is current with the working tree. ⚠ The mod is currently
  **disabled** in the MO2 `Default` profile — re-enable before the §0 run.

---

## 7. TODO list, ordered by reverse-engineering risk

1. ✅ Per-frame tick (SFSE TaskInterface) — **done + verified in-game**.
2. ✅ Input event source (observe-only) — **done + fully verified in-game**
   (all five §0 markers, runs of 2026-06-12 19:52 + 20:20).
3. ✅ Real Ultralight backend offscreen (Phase 1) — **done + verified
   in-game 2026-06-12** (rendered test page PNG-dumped, bridge round-trip
   proven; see §4 "Ultralight backend" and renderer-plan.md).
4. ✅ Route to Starfield's `ID3D12Device` + direct queue — **runtime-PROVEN
   on 1.16.244, 2026-06-12** (hook-free, in `OSF RE/`: D3D12DeviceProbe,
   anchored on REL::ID(944397); chain + proof in
   [reverse-engineering-notes.md](reverse-engineering-notes.md) §2).
5. ✅ Present timing — **DECIDED 2026-06-12**: hook `IDXGISwapChain::Present`
   slot 8 (runtime-proven in `OSF RE`, RenderPresentProbe re-anchored to
   ID 141996; rationale + two-swapchain caveat in RE notes §2).
6. ✅ Overlay composition — **first light done & user-verified 2026-06-12**
   (present-time alpha-blended draw; descriptor heaps, resource states,
   command ring all working). Polish remaining: HDR/DRS, aspect/scaling,
   frame-gen swapchain selection, Steam/ReShade/RTSS coexistence
   (renderer-plan.md Phase 3).
7. ⏳ Input **routing + consumption** (Phase 4): feed observed input into
   `View::FireMouseEvent`/`FireKeyEvent`, focus model, block the game from
   acting on captured input while the overlay has focus, text input
   (`CharacterEvent`), cursor routing.

Risk note: items 4–6 are the genuinely hard RE. Items 1–2 needed only source
reading (SFSE + CommonLibSF), which is why they're done first.

---

## 8. File map (where things live)

```
src/
  main.cpp                     SFSE entry macros -> Plugin::OnPreLoad/OnLoad
  core/      Plugin, Paths, Log, Config, Version
  runtime/   Runtime, ViewManager, ViewManifest, MessageBridge, Json
  render/    IWebRenderer + Null / Mock / Ultralight(stub)
  composite/ ICompositor + Null / D3D12(stub)
  input/     InputRouter, InputTypes, MenuEventSink, UiInputHook
  platform/  WindowsPlatform (isolated Win32)
data/StarfieldWebUI/   config.json + views/test/{manifest,index.html,style.css,main.js}
docs/        architecture, renderer-plan, security-model,
             reverse-engineering-notes, HANDOFF (this file)
```
