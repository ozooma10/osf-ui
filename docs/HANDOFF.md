# StarfieldWebUI — Resume / Handoff

**Last updated:** 2026-06-12
**Status:** Phase 0 skeleton complete + TODO #1 (tick) and #2 (input) implemented.
Everything builds; **nothing has been verified in-game yet** (no game access at
time of writing).

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
> ⚠ As of this writing the repo's `origin` still points at the **template**
> repo and changes are **uncommitted**. Before transferring, either commit and
> push to your own remote, or copy the working tree directly. See §6.

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

## 3. Current state — what works (compiles; unverified in-game)

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
| **TODO #1** — per-frame `Runtime::Tick()` via SFSE `TaskInterface` | ✅ implemented, ⏳ unverified in-game |
| **TODO #2** — input observation (UiInputHook) + menu events (MenuEventSink) | ✅ implemented, ⏳ unverified in-game |
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

### Input (TODO #2 — done)
- **Key space:** Starfield keyboard `ButtonEvent::idCode` = DirectInput scan
  codes = `SFSE::InputMap` space (0–255 kbd, 256+ mouse, 266+ gamepad).
  **NOT Windows VK codes** — an earlier VK-based resolver was a bug, fixed.
- [UiInputHook.cpp](../src/input/UiInputHook.cpp): the project's ONLY hook.
  A vfunc swap on `RE::UI::VTABLE[0]` slot 1 (`PerformInputProcessing`) using
  CommonLibSF's maintained AddressLib ID. **Observe-only**: reads button
  events, feeds `InputRouter`, always forwards the unmodified queue. Gated by
  config `inputSource` (`"none"` = off in code; shipped config uses `"ui"`).
  Install is **one-way** (no safe un-swap once other overlays chain on) —
  disabling uses a pass-through flag, not an unhook.
- [MenuEventSink.cpp](../src/input/MenuEventSink.cpp): hook-free
  `BSTEventSink<MenuOpenCloseEvent>` registered on the UI singleton via the
  documented `RegisterSink` API at `kPostPostDataLoad`.
- Toggle path is live end to end: `F10` → router → `Runtime::ToggleVisible()`
  → mock frames flow to the null compositor.

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
   - `FrameTick: first per-frame task received` (proves #1)
   - `MenuEventSink: registered` and `UiInputHook: installed` (proves #2 wired)
4. Press **F10** → expect `Runtime: overlay visibility -> true` and
   `NullCompositor: first frame submitted`.
5. **Answer the open RE questions** and record them in
   [reverse-engineering-notes.md](reverse-engineering-notes.md):
   - Does the tick pump at main menu / while paused / during loading screens?
   - What thread does input arrive on? Is `heldDownSecs == 0` a reliable
     "initial press" marker? Do menu names log as expected?

---

## 6. ⚠ Before/after the transfer — git hygiene

- The working tree has **uncommitted changes** and `origin` points at the
  upstream template. To preserve work across machines, do ONE of:
  - `git remote set-url origin <your-repo>` then commit + push, **or**
  - copy the whole `StarfieldWebUI\` folder (including `lib/` submodules) to
    the new machine.
- Do not run `git checkout --`/reset/stash on anything you didn't dirty —
  several workspace repos are intentionally dirty.
- `build/` and any `vsxmakeXXXX/` dirs are regenerable; no need to copy them.

---

## 7. TODO list, ordered by reverse-engineering risk

1. ✅ Per-frame tick (SFSE TaskInterface) — **done**, verify in-game.
2. ✅ Input event source (observe-only) — **done**, verify in-game.
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
