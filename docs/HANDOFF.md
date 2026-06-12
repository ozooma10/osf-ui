# StarfieldWebUI — Resume / Handoff

**Last updated:** 2026-06-12 ~18:00 (mid machine switch)
**Status:** Phase 0 + TODO #1 (tick) **verified in-game**. TODO #2 (input)
verified through three test runs today: the save-load crash is fixed (§4a),
menu events and input observation are proven working. **ONE verification run
remains** (see §0): the F10 toggle never fired because the key space was
wrong (VK, not DIK — fixed in code, built, deployed, but not yet re-tested).
Game is **1.16.244.0** (patched 2026-06-11; SFSE 0.2.21, versionlib-1-16-244
present in the AIO address library mod).

---

## 0. IMMEDIATE next step (one run, ~3 min)

Everything is already built and deployed to `MO2\mods\StarfieldWebUI`.
Launch via MO2+SFSE, load a save, press **F10**, check
`Documents\My Games\Starfield\SFSE\Logs\StarfieldWebUI.log` for:

1. `Runtime: toggleKey 'F10' resolved to VK code 0x79`  (new line — proves
   the rebuilt DLL is the one loaded; the old one said `InputMap code 0x44`)
2. no `UI layout guard FAILED`
3. `MenuEventSink: registered` / `UiInputHook: installed`
4. on F10: `Runtime: overlay visibility -> true` +
   `NullCompositor: first frame submitted`
5. bonus: click the mouse once — expect `OnMouseButton(0, true)` AND
   `OnMouseButton(0, false)` (the release was previously eaten; fixed).

If all five hold, TODO #2 is fully verified end-to-end → mark it ✅ in §3/§7
and move to TODO #3 (Ultralight backend, no game needed) or #4 (D3D12 RE).

This file is the single place to re-orient after switching machines. Read it,
then read [architecture.md](architecture.md) and
[reverse-engineering-notes.md](reverse-engineering-notes.md).

---

## 1. What this project is

An SFSE/CommonLibSF plugin (`StarfieldWebUI`) that will eventually host
HTML/CSS/JS UI views inside Starfield (Prisma-UI-*inspired*, no Prisma code).
Built from [libxse/commonlibsf-template](https://github.com/libxse/commonlibsf-template),
C++23 + XMake, GPL-3.0.

Repo root: `C:\Modding\Starfield\StarfieldWebUI`
(part of the larger multi-repo Starfield modding workspace.)

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
  `C:\Modding\Starfield\MO2\mods\StarfieldWebUI`). On the new machine set
  `XSE_SF_MODS_PATH` or `XSE_SF_GAME_PATH` to get auto-deploy.

### Optional Ultralight build (currently a stub backend)
```bat
set ULTRALIGHT_SDK_DIR=C:\path\to\ultralight-sdk
xmake f --with_ultralight=true
xmake build
```
Fails with a clear message if the SDK dir is missing. **Default build must
stay Ultralight-free.**

---

## 3. Current state — what works

| Area | State |
|---|---|
| SFSE preload/load lifecycle + logging | ✅ implemented |
| Paths (resolved relative to the DLL, MO2-VFS safe) | ✅ |
| Config load (`config.json`, defensive, defaults on missing) | ✅ |
| View manifest discovery + validation | ✅ |
| Renderer abstraction: Null, Mock (CPU RGBA test pattern), Ultralight stub | ✅ |
| Compositor abstraction: Null, D3D12 stub (fails Initialize by design) | ✅ |
| JSON message bridge (whitelist: close/log/ping/setVisible) | ✅ |
| Sample `test` view (HTML/CSS/JS, runs standalone in a browser too) | ✅ |
| **TODO #1** — per-frame `Runtime::Tick()` via SFSE `TaskInterface` | ✅ implemented, ✅ **verified in-game 2026-06-12** |
| **TODO #2** — input observation (UiInputHook) + menu events (MenuEventSink) | ✅ verified in-game (menu events + key/mouse observation); ⏳ F10 toggle re-test pending (§0) |
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

### Hard rules being honored
- No invented addresses/offsets/vtables/menu names. The single vtable ID comes
  from CommonLibSF, not from us.
- D3D12 compositor `Initialize()` **fails on purpose** so nothing mistakes it
  for a working present path.
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
   `HUDMessagesMenu`, `CursorMenu`). Still open: tick during pause menu and
   loading screens (check DEBUG heartbeat continuity next session).

---

## 6. ⚠ Before/after the transfer — git hygiene

- `origin` = `https://github.com/ozooma10/osf-ui.git` (already correct).
- **Uncommitted as of 2026-06-12 ~18:00** — all of today's crash fix +
  verification work:
  - `lib/commonlibsf` — submodule bumped `12d665b` → `4c48ed4` (upstream
    libxse HEAD with PR #26–28; publicly fetchable, safe to push the pin)
  - `src/input/UiInputHook.{h,cpp}` — VTABLE[10] + `VerifyUiLayout()` guard
  - `src/core/Plugin.cpp` — guard gates all UI integration
  - `src/input/InputRouter.cpp`, `src/input/InputTypes.h` — VK key space
  - `src/runtime/Runtime.cpp` — log wording (VK code)
  - `src/main.cpp` — Debug log level for crash forensics
  - `docs/HANDOFF.md`, `docs/reverse-engineering-notes.md` — findings
  - `tools/parse_versionlib.py` — **untracked, add it** (versionlib → offset
    resolver; proved the VTABLE[10] claim)
- To transfer: commit + push (suggested message: "Fix UI layout crash:
  update commonlibsf, add layout guard, correct key space to VK"), or copy
  the whole folder including `lib/`.
- Do not run `git checkout --`/reset/stash on anything you didn't dirty —
  several workspace repos are intentionally dirty.
- `build/` and any `vsxmakeXXXX/` dirs are regenerable; no need to copy them.
- The deployed DLL in `MO2\mods\StarfieldWebUI` is current with the working
  tree (built 2026-06-12 ~17:55). The mod must be **enabled in MO2** (it is
  on this machine).

---

## 7. TODO list, ordered by reverse-engineering risk

1. ✅ Per-frame tick (SFSE TaskInterface) — **done + verified in-game**.
2. ✅ Input event source (observe-only) — **done + verified** except the F10
   toggle run (§0).
3. ⏳ Implement the real Ultralight backend offscreen (Phase 1 — SDK work,
   no game RE). Stub + build wiring already in place behind `with_ultralight`.
4. ⏳ Prove a route to Starfield's `ID3D12Device` + direct queue (do this in
   `OSF RE/` per its proof rules).
5. ⏳ Present timing decision: engine end-of-frame fn vs `IDXGISwapChain3::Present`
   hook.
6. ⏳ Overlay composition: descriptor heaps, resource states, HDR/DRS,
   Steam/ReShade/RTSS coexistence.
7. ⏳ Input **consumption** (block the game from acting on captured input while
   overlay has focus) + text input (`CharacterEvent`) + cursor routing.

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
