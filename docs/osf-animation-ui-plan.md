# OSF Animation UI — plan & resume notes

**Status:** In progress · **Date:** 2026-06-30 · **Spans two repos:** `OSF UI`
(this one) and `OSF Animation` (`C:\Modding\Starfield\OSF Animation`).

This is a self-contained handoff so work can resume on another machine. It does
**not** rely on any local Claude auto-memory (that does not transfer). Everything
needed to continue is here or in the linked files.

---

## 0. TL;DR — where we are, what's next

- **Goal:** make **OSF Animation** the flagship in-game UI consumer of **OSF UI**
  (the Ultralight/Chromium HTML overlay for Starfield).
- **Decided MVP:** a **scene browser + starter** only — *not* the live "Director
  HUD" yet. (Browse installed scenes, filter, pick a target via crosshair,
  launch — with honest success/failure feedback.)
- **Decided architecture:** **native-to-native** over OSF UI's `MessageBridge`,
  via a **new exported OSF UI bridge API** ("Option B"). **No Papyrus bridge.**
- **Already written:** [`docs/native-plugin-api.md`](native-plugin-api.md) — the
  full design/RFC for that bridge API (`OSFUI_RequestBridge` / `IOSFUIBridge`,
  with a copyable `sdk/OSFUI_API.h` in its Appendix A).
- **Resume here →** §7 build order. The immediate next step is implementing the
  bridge API inside OSF UI (`src/api/` + `sdk/OSFUI_API.h`), then the OSF
  Animation consumer side + the view.

---

## 1. The decisions (and why)

1. **MVP = scene browser/starter first** (not the Director HUD). It's the
   screenshot-worthy player flow, and it avoids OSF Animation's two delicate
   areas: the async `SceneEvent` relay (UAF history) and the in-scene
   capability/locked gating. The HUD is a strong phase 2.
2. **Option B (exported bridge API) over Option A (in-tree module).** OSF UI has
   **no** public API for sibling plugins today; *something* native must register
   the `osf.*` bridge commands. Option A would compile OSF-specific code into
   `OSFUI.dll` (couples the repos, doesn't scale). Option B adds a small exported
   API so **OSF Animation's own DLL** registers the commands and answers them
   in-process. Benefits: clean separation; the catalog comes from OSF Animation's
   **live registered scene set** (better than an optimistic disk scan); and it
   **deletes the Papyrus bridge** entirely (bridge handlers run on the game main
   thread, where OSF's natives must run anyway).
3. **Lead player-first.** The cheap integrator/author panels (event console, pack
   validator) are a fast follow, not in this MVP.

---

## 2. Key facts — OSF UI (the framework)

Audited 2026-06-30. All file refs are in this repo unless noted.

- **What it is / works today:** SFSE/CommonLibSF plugin; renders real HTML/CSS/JS
  via Ultralight offscreen, uploads to a GPU texture on the game's own D3D12
  device, draws it over the image through an `IDXGISwapChain::Present` slot-8
  hook. Fully interactive (game input frozen, VK keyboard routed in, raw-mouse
  virtual cursor). Ships a schema-driven MCM-style settings view. **Verified
  in-game on Starfield 1.16.244.** Phase 5b.
- **JS↔native bridge:** `window.osfui` exists only if the view's `manifest.json`
  has `permissions.nativeBridge: true`. Surface = just `postMessage(jsonString)`
  (web→native) and `onMessage = fn` (native→web). Envelope both ways:
  `{type, payload}`. Web→native is **only** `type:"ui.command"`,
  `payload:{command,...}`; **exact-match whitelist, no generic call-native
  hatch**. Native→web: `MessageBridge::SendToWeb(viewId, type, payload)`.
  Handshake message `runtime.ready {game, plugin, version, bridgeVersion}`;
  `bridgeVersion = "0.1"` and explicitly unstable. **Command handlers run on the
  game MAIN thread.**
- **No public API for sibling plugins (this is the gap we're closing).** Commands
  register in C++ only (`RegisterPlatformCommands` + each
  `IUiModule::RegisterCommands`); modules are hardcoded in
  `Runtime::BuildModules()` (only `SettingsModule`). No exported symbol, no
  `.def`. The SFSE listener is registered for the `"SFSE"` sender only and
  ignores custom messages; OSF UI never `Dispatch`es → a sibling plugin cannot
  drive it today.
- **One no-code path for a sibling mod:** drop `OSFUI/views/<id>/`
  (manifest + html/js) into OSF UI's data dir → `ViewManager` auto-loads it as a
  static view. Anything native still needs the new API.
- **Sandbox blocks JS disk reads.** `SandboxFileSystem::Resolve`
  (`src/render/UltralightWebRenderer.cpp` ~169) rejects absolute paths + `..` and
  re-bases everything under `OSFUI/views`. The `filesystem` manifest permission
  is a reserved **no-op**; `network` is forced off. **A view cannot read another
  mod's `Data\OSF\*.osf.json` — the catalog MUST come over the native bridge.**
- **Key files:** `src/runtime/MessageBridge.{h,cpp}`,
  `src/runtime/Runtime.{h,cpp}`, `src/runtime/UiModule.h`, `src/core/Version.h`
  (`kBridgeProtocolVersion="0.1"`, `kPluginVersion="0.1.0"`,
  `kDataFolderName="OSFUI"`), `src/core/Plugin.cpp` (SFSE lifecycle +
  `FrameTickTask` main-thread tick), `src/render/UltralightWebRenderer.cpp`
  (bridge inject ~878, sandbox ~169), `src/runtime/ViewManager.{h,cpp}`.

---

## 3. Key facts — OSF Animation (the consumer)

Repo: `C:\Modding\Starfield\OSF Animation`. These are the facts the UI depends on
(verified by reading the code, 2026-06-30).

- **Catalog source.** There is **no scene-enumeration native** (Papyrus *or*
  C++): `Matchmaker::FindIds` and `SceneRegistry::ForEachDef` are internal C++
  only. Two ways to build a catalog: (a) read `Data\OSF\**\*.osf.json` off disk
  (recursive, loose files only, **JSONC** — `//` comments; `schema` must == 1;
  dedupe by lowercased `id`, first-filename wins); or **(b, preferred in Option
  B)** answer from OSF Animation's **live in-memory registry** (its own DLL can
  walk `ForEachDef`).
- **Scene format (what a card shows / filters on):** `id` (required — the launch
  key; **bare, flat, case-insensitive**), `name` (title; defaults to `id`),
  `tags[]`, `roles[]` (`name`, `gender` = any/male/female, `filters.keyword/race`
  as form-refs), and an **`anchor` block = "requires furniture"** (its presence;
  `RequiresAnchor()`). Actor count = `roles.length` or first-stage clip count.
  **There are NO `description`, `author`, or `thumbnail` fields** — so the MVP
  browser is text + filters, not an image gallery. The `anchor` block is
  **undocumented**; spec it from `src/Registry/SceneRegistry.cpp:953`
  (`ParseAnchorBlock`) and `:265` (`RequiresAnchor`), not from
  `docs/SCENE_SCHEMA.md`.
- **Launch path (native, in Option B).** OSF Animation already exports a native
  C++ API: **`OSF_RequestSceneAPI`** (`src/API/OSFSceneAPI.h`, single copyable
  header). Vtable `IOSFSceneAPI` covers `StartScene(RE::Actor* const*, count,
  sceneId, OSFStartOptions)` → `int32` handle (`0` = fail), `StartSceneRoles`,
  `StartSceneFiles`, `StopScene`, `SetStage`/`Advance`/`Navigate`, solo
  `Play/Stop/SetSpeed`, and queries. **It does NOT cover enumeration or
  matchmaking-by-tags** — so the catalog still comes from the registry/disk, and
  furniture/tag-first starts may need OSF Animation to extend its own API or read
  the registry. `OSFStartOptions` is a POD with tri-states
  (`1`/`0`/`-1`=inherit), `camera[64]`, `loopScale` (clamped to 20), `anchorRef`
  (`RE::TESObjectREFR*`) for furniture-bound scenes. It passes only PODs +
  **opaque** `RE::Actor*`/`RE::TESObjectREFR*` (pointer-identity only).
- **Equivalent Papyrus natives** (reference; not used in Option B): `OSF.IsReady`
  / `GetVersion` / `ReloadPacks`; `OSFCompat.GetCrosshairActor`/`GetCrosshairRef`;
  `OSF.StartScene` / `OSFAdvanced.StartSceneRolesEx` / `StartSceneByTags[Query]` /
  `StartSceneAtAnchor`; `OSFTypes:SceneOptions` (tri-states via
  `OSF.INHERIT()=-1`/`OFF()=0`/`ON()=1`); failure = handle `0`, reason via
  `OSFAdvanced.GetSceneLoadErrors()`/`GetMissingClipRefs()`.
- **Version inconsistency to fix:** `OSF Animation\xmake.lua` says `0.1.1` but the
  changelog is `0.2.0` — the UI's version display reads `GetVersion()`, so bump
  it before any demo.
- **Key files:** `src/API/OSFSceneAPI.{h,cpp}`,
  `src/Registry/SceneRegistry.{h,cpp}`, `src/Papyrus/OSFScript.cpp`,
  `dist/OSF/*.osf.json`, `dist/Scripts/Source/OSFTypes.psc`,
  `docs/RFC-native-api.md`.

---

## 4. Target architecture

```
OSF Animation view (HTML/CSS/JS)        ships in OSFUI/views/osf/ (nativeBridge:true)
        │  window.osfui.postMessage / onMessage   ({type, payload} JSON)
        ▼
OSF UI  MessageBridge  ── ui.command ──▶  (NEW) OSFUI bridge API  ── CommandFn ──▶  OSF Animation DLL
        ▲                                  (OSFUI_RequestBridge)                    command handlers
        └────────── SendToWeb ◀──────────────────────────────────────────────────  (run on main thread)
                                                                                          │
                                                          in-process (its own commonlibsf) ▼
                                                          OSF Animation SceneRuntime / registry
```

- **Catalog:** view sends `osf.catalog.get` → OSF Animation handler serializes
  its live registry → replies `osf.catalog.data`.
- **Launch:** view sends `osf.launch {sceneId, cast, opts}` → handler resolves
  the crosshair `RE::Actor*` (in-process) and calls `SceneRuntime::Start` (or its
  own `OSF_RequestSceneAPI`) → replies `osf.launchResult {ok, handle, error?}`.
- **The cross-DLL surface is JSON text only** — no STL/`nlohmann`/`RE::*` types.
  That makes the bridge **independent of the commonlibsf pin** (see §6).

---

## 5. The bridge API to build (summary — full spec in `native-plugin-api.md`)

- **Export:** `extern "C" __declspec(dllexport) OSFUI_RequestBridge(uint32_t
  abiVersion)` in `OSFUI.dll`; fetched via `GetModuleHandle("OSFUI.dll")` +
  `GetProcAddress`, after SFSE `kPostLoad`. Versioned `(MAJOR<<16)|MINOR`,
  append-only vtable (mirrors OSF Animation's `OSF_RequestSceneAPI`).
- **`IOSFUIBridge`:** `RegisterCommand(command, CommandFn, user)` /
  `UnregisterCommand`; `SendToWeb(viewId, type, payloadJson)`; `IsBridgeReady` +
  `SetReadyCallback`; version/handshake getters. All ABI-safe (C strings +
  function pointers + POD).
- **Ready gate (the robust part):** the API keeps its own registry independent of
  `MessageBridge` lifetime, applies it on `Runtime::Tick` when a bridge-enabled
  view comes up, and re-applies on re-creation — so a consumer can register
  before anything exists and never re-register. A view is still required to exist
  (it's what triggers bridge construction).
- **Reserved prefixes** (`ui./runtime./game./settings.`) refused; consumers use
  `osf.*`.

---

## 6. ABI / commonlibsf note

Both repos are **C++23 / MSVC** on the same `ozooma10/commonlibsf` **`forge`**
fork. OSF UI pins `5df499f`; OSF Animation pins `8fdcac4` (~24 forge commits
ahead; `5df499f` is a direct ancestor → clean fast-forward). **Because the bridge
API carries only JSON text, the pin mismatch does NOT affect it** — the
fast-forward is *not* required for this work. (It would only matter if some API
ever passed engine types by layout; don't. OSF Animation's in-process scene work
uses its own commonlibsf, so no engine pointers cross the bridge.)

---

## 7. Build order (RESUME HERE)

1. **OSF UI — implement the bridge API** per `docs/native-plugin-api.md` §8:
   - `sdk/OSFUI_API.h` (copy Appendix A verbatim).
   - `src/api/BridgeApi.{h,cpp}` (the `IOSFUIBridge` singleton + own registry +
     main-thread op queue + the `CommandFn`→`std::function` trampoline).
   - `src/api/Exports.cpp` (`extern "C"` factory, MAJOR check).
   - Wire-in: in `Runtime::Initialize` after bridge/module setup, call
     `API::BridgeApi::Get().OnBridgeReady(_bridge.get())`; in `Runtime::Tick`,
     `API::BridgeApi::Get().PumpMainThread()`.
2. **OSF UI — prove the round-trip:** a throwaway `dev.echo` command + a test
   view that sends it and renders the reply. Verify deferred registration
   (register before the view's DOM is ready) and reserved-prefix rejection.
3. **OSF Animation — consumer side:** copy `OSFUI_API.h`; in its SFSE `kPostLoad`
   handler `RequestBridge()` (skip UI cleanly if `nullptr`), `SetReadyCallback`,
   and `RegisterCommand` the `osf.*` set (§8). Handlers answer the catalog from
   the registry and launch in-process; reply via `SendToWeb`.
4. **OSF Animation — the view:** ship `OSFUI/views/osf/` (manifest with
   `permissions.nativeBridge: true` + HTML). Build the MVP UI (§9). It speaks
   only `window.osfui` — sourced over the bridge, not disk. The page also runs
   standalone in a normal browser (degraded mode) for fast dev.
5. **(v1.1) `RegisterViewRoot`** so OSF Animation can ship its view under its OWN
   mod folder instead of `OSFUI/views/` (needs multi-root `ViewManager` + per-view
   sandbox base — see `native-plugin-api.md` §9).
6. **Housekeeping:** bump OSF Animation `xmake.lua` version (0.1.1 → 0.2.0);
   optionally fast-forward OSF UI's commonlibsf pin (not required, §6).

---

## 8. Message contract (`osf.*`)

| Direction | `type` / `command` | payload |
|---|---|---|
| web→native | `osf.catalog.get` | — |
| native→web | `osf.catalog.data` | `[{id,title,tags[],actorCount,genders[],requiresFurniture}]` |
| web→native | `osf.pickCrosshair` | `{slot:"actor"\|"furniture"}` |
| native→web | `osf.pick` | `{slot,token,name,formId,valid}` |
| web→native | `osf.launch` | `{sceneId,mode,castTokens[],roleNames?,furnitureToken?,opts}` |
| native→web | `osf.launchResult` | `{ok,handle,sceneId,error?}` |
| web→native | `osf.stop` | `{handle}` |
| native→web | `runtime.ready` | `{game,plugin,version,bridgeVersion}` (platform; gate on `bridgeVersion`) |

Targeting is **crosshair-only** for MVP (`GetCrosshairActor`/`GetCrosshairRef`).
`RE::Actor*`/`RE::TESObjectREFR*` stay in OSF Animation's process; the view holds
opaque integer **tokens** the handler maps back. Furniture-required scenes
(`requiresFurniture`) disable Play until a furniture token is picked.

---

## 9. The MVP UI (screens)

- **Scene browser ("Cinema"):** text card grid — title, tag pills, actor-count
  badge, "needs furniture" badge, optional gender badge. Filter rail: search +
  tag chips + actor-count + needs-furniture + "fits my cast". Group by pack/file.
- **Cast tray (docked):** Player slot (always, token `-1`), "Use crosshair"
  partner slot, "Use crosshair furniture" slot. Each pick round-trips through the
  bridge and shows the returned name.
- **Detail / launch panel:** title, tags, roles, requirements; a *minimal*
  `SceneOptions` block (Strip / LockPlayer tri-states default inherit, Camera
  dropdown, Speed) and **Play** (disabled with a reason when requirements aren't
  met). A Stop control for the last-launched handle.
- **Status strip:** ready light + `version`, scene count, **Refresh**
  (`ReloadPacks`), and a load-error notice.
- **Failure feedback:** a launch returning handle `0` shows the real reason
  (pull `GetSceneLoadErrors`/`GetMissingClipRefs`).

---

## 10. Open decisions

- **View distribution:** drop into `OSFUI/views/` (v1) vs `RegisterViewRoot`
  (v1.1, cleaner per-mod). Recommend v1 disk-drop first.
- **Auto-bind vs named roles** for MVP launch: order-based `StartScene` auto-bind
  is fine for MVP (lean on the gender filter + clear errors); named-role drag
  (`StartSceneRolesEx`) is a fast-follow.
- **Confirm Option B** (recommended) vs Option A in-tree module.

---

## 11. Pointers

- This repo: [`docs/native-plugin-api.md`](native-plugin-api.md) (the bridge API
  spec + copyable header), `docs/architecture.md`, `docs/authoring-views.md`,
  `docs/security-model.md`, `docs/ROADMAP.md` (note: a public native API was
  prototyped 2026-06-24 and **reverted** 2026-06-25; this revives it under the
  OSF UI brand).
- OSF Animation repo: `src/API/OSFSceneAPI.h`,
  `src/Registry/SceneRegistry.{h,cpp}`, `src/Papyrus/OSFScript.cpp`,
  `dist/OSF/*.osf.json`, `dist/Scripts/Source/OSFTypes.psc`.
- **Reminder:** local Claude auto-memory does not transfer between machines —
  this doc + `native-plugin-api.md` are the source of truth.
