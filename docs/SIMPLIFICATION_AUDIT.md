# OSF UI — Simplification Audit

> **Purpose:** identify *accidental* complexity that can be removed to make the codebase
> more intuitive, while explicitly protecting *load-bearing* complexity (hard-won crash
> fixes). This is a simplification/quality report, **not** a bug hunt.
>
> **Generated:** 2026-07-24 · **Anchored to commit:** `e4db141` (working tree, with the
> uncommitted `src/api/BridgeApi.h` edit present).
> **Method:** 14 subsystem analyzers → adversarial per-finding verification (each proposal
> re-checked against the real code, `CHANGELOG.md`, and git history for *behavior-preservation*
> and *load-bearing* status) → dedup + synthesis. 60 findings; **27 confirmed, 30 refined,
> 3 rejected.**
>
> **➡ See also [SIMPLIFICATION_PLAN.md](./SIMPLIFICATION_PLAN.md)** — the fused, sequenced
> execution plan that reconciles this audit with two later reviews (a god-object review and a
> thread-defense-retirement review) and adds items outside this audit's scope.
>
> **⚠ Scope caveat:** this audit covered only `src/` and `frontend/`. It did **not** analyze
> `tools/webview2_host/` (which contains `HostApp.cpp`, the 2935-line single largest
> concentrated-responsibility file in the repo), `docs/`, or the bridge protocol-name surface.
> Those are covered additively in the plan. **One correction from verification:** do-not-touch
> #10 below has been revised — the pause-menu liveness/count gates and the `kListStableTicks`
> debounce are *separable* (git proves the gates predate the debounce), so only the gates are
> load-bearing; the debounce is validate-then-remove. See §2.1 of the plan.

---

## How to read this report (reviewer, start here)

1. **Line numbers drift.** They were captured at `e4db141`. Re-anchor by symbol/content, not
   by line, if HEAD has moved. Every item names the symbol(s) involved.
2. **"Refine" ≠ "confirm".** 30 of the 57 actionable items came back as *refine*: the obvious
   simplification is **subtly unsafe** and the item text below already encodes the corrected,
   safe approach. Do not "simplify further" than what each item says — the guardrails are there
   for a reason (a rename that breaks a wire contract, a helper that won't compile, an extraction
   that reintroduces a data race, a dropped documented behavior).
3. **Respect the Do-Not-Touch list (§7).** Several things that *look* over-engineered are
   protecting against real shipped CTDs (FrameGen, BetterConsole, pause-menu VM, thread affinity).
   Removing them regresses known crashes.
4. **Suggested landing order:** §3 quick wins first (cheap, independently reviewable), then §4
   high-impact items one commit each. The two CTD-adjacent / race-sensitive items
   (`ReconcileList`, keybinds `useCapture`) must be isolated commits with a test pass + in-game
   smoke check.

### Load-bearing systems the reviewer must keep in mind

These fixed real shipped crashes; complexity around them is intentional:

- **Thread affinity / `MainThreadMenuPump`** — SFSE tasks run on a render-graph *worker*, not the
  main thread; per-tick `RE::UI`/Scaleform/GFx access must be marshalled to the main thread.
- **FrameGen / FSR3 / DLSS-G draw suspension** — overlay draws are globally suspended while
  frame-gen paces its swapchain (CTD reports #2/#4).
- **BetterConsole swapchain-hook workaround** — the windowless probe on a private queue avoids
  tripping BetterConsole's inline hook.
- **Pause-menu list stability debounce (`kListStableTicks=3`)** — guards the report #3 Scaleform
  VM fault.
- **`UiPassSeam` under-Scaleform draw** — draws inside the engine's Scaleform UI pass for FG support.

---

## 1. Executive summary

The over-engineering is **overwhelmingly accidental duplication and missing shared helpers**,
not genuinely wrong abstraction. The same primitives (ASCII string ops, `targetVersion` parsing,
a qualified-id split, a React state+ref mirror, a controlled-text re-seed) are hand-rolled at
many sites, and several subsystems repeat a fixed block (reload-a-view, key-conflict collection,
vtable patching, Papyrus dispatch, outbound pipe JSON) 3–5 times.

The biggest structural levers:
- Carve the self-contained **diagnostics** subsystem out of the ~2700-line `Runtime` god object.
- Hoist the imperative **benchmark** engine into a hook.
- Collapse two copies of a race-sensitive **keybind-capture** state machine onto the already-extracted `useCapture`.

A meaningful amount of **dead scaffolding** can simply be deleted. The key caveat: many *refine*
verdicts exist because the proposed mechanism was subtly unsafe — follow the refined text, not the
naive idea. A handful of "simplifications" are hard-won crash fixes / per-patch tripwires and are
listed in §7 as **do-not-touch**.

**Metrics:** 60 findings — 27 confirm · 30 refine · 3 rejected (load-bearing).

---

## 2. Priority index

### Top quick wins (low effort, clearly worth it)
- Delete the dead dirty-rectangle machinery *(§6.1)*
- Delete `rail.ts`'s dead `cycleRail` *(§6.7)*
- `interpolateEnglish` helper — dedupe two byte-identical `t()` copies *(§5.10)*
- `isModified` should reuse `sameValue` *(§5.11)*
- Single owner for `HOME_ID` *(§5.9)*
- `Json::CheckFormatVersion` for the 'declares newer' INFO block *(§4b.3)*
- `HudCard` should reuse the shared `Mark` component *(§5.6)*
- `useCommittedText` hook — controlled-text re-seed *(§5.5)*
- Delete dead `MenuController::ToggleDefault` *(§6.2)*
- Remove the `g_creatorReady` dead kill-switch *(§6.3)*
- Delete vestigial `[wheel-probe]` logging *(§6.4)*
- Delete the orphaned `LoadLibrary` doc comment *(§6.6)*
- `DispatchOne` helper for the Papyrus static-vs-method dyad *(§4c.7)*

### Top high-impact (biggest structural simplifications)
- Extract a `RuntimeDiagnostics` collaborator from the ~2700-line `Runtime` *(§4a.1)*
- Reuse `useCapture` in keybinds — deletes ~90 lines of a duplicated race-sensitive state machine *(§5.2)*
- `core/StringUtil.h`: `Trim` / `ToLowerAscii` / `EqualsCaseInsensitiveAscii` (6 sites) *(§4b.1)*
- Hoist the benchmark engine into a `useBenchmark()` hook *(§4a.3)*
- `useStateRef` hook — the state+ref+dual-write triple (~8 sites) *(§5.1)*
- Delete the `InputRouter` class and inline routing into `OnHostKey` *(§6b.1)*
- Decompose the 175-line `ReconcileList` — CTD-adjacent, careful *(§4a.4)*

---

## 3. Theme A — Split the god objects and oversized functions

*A few units mix genuinely unrelated concerns in one body, forcing a reader to hold an entire
engine in their head to follow any one part. Extracting the self-contained halves is
behavior-preserving code-motion.*

### 4a.1 — Extract a `RuntimeDiagnostics` collaborator from the ~2700-line Runtime · **L / high**
`src/runtime/Runtime.cpp`, `src/runtime/Runtime.h`
Move the diagnostics producers and the four `_diag*` fields into a `RuntimeDiagnostics`
collaborator with read access to settings/views/compositor/renderer/localization/config/uptime.
Runtime forwards **three** edges (`Pump` from `Tick`, plus `OnRendererHealth` and
`ReportViewLoadDiagnostic` — they are separate entry points, **not** one Tick site).
**Do NOT** extract the gamepad `DrainEngineInput` half (see §7).

### 4a.2 — Extract `OverlayValues` from the 260-line `AddSchema` · **M / medium**
`src/runtime/SettingsStore.cpp`
Move the `456–534` overlay block into `std::size_t OverlayValues(Mod&, const json& saved)` that
**returns the setting count** (the count log at ~`550` consumes it). Keep the whole accounted-set
+ alias-adoption + preserved-bag range together; keep `saved` a caller local (still read at ~`542`)
and leave version-bookkeeping before the call.

### 4a.3 — Hoist the benchmark engine into a `useBenchmark()` hook · **L / medium**
`frontend/src/views/osfui/benchmark/App.tsx`
Move `exercise`/`canvasExercise`/`ActiveRun`/`tick`/`timer`/`runSuite` and the refs into a
`useBenchmark()` hook, leaving ~150 lines of JSX. Move the `[]`-deps effect cleanup
(`cancelAnimationFrame` + `clearInterval` + suite-token bump) and the ref-vs-state split verbatim.

### 4a.4 — Decompose the 175-line `ReconcileList` (⚠ CTD-adjacent) · **M / medium**
`src/input/PauseMenuEntry.cpp`
Extract `NavigateToList`/`InstallPressListener`/`InjectEntry`, **but**: keep the debounce-advance
in the parent **before** all guards; preserve the per-hop warn asymmetry (root-not-ready returns
silently; MainPanel/MainList miss warn); keep the found-ours scan + `foundOurs` early-return in the
parent (`InjectEntry` takes an already-populated `newList`). Ship as an isolated reviewed commit
with a native test pass and an in-game pause-menu smoke check — **this guards the confirmed report
#3 crash** (see §7).

---

## 4b. Theme B — Fill the missing shared-helper gaps (primitives reinvented across files)

*The same primitive is re-derived in many translation units with subtly different shapes, so a
reader must re-verify at every call site and any rule change must be edited everywhere.*

### 4b.1 — `core/StringUtil.h`: `Trim` / `ToLowerAscii` / `EqualsCaseInsensitiveAscii` (6 sites) · **M / high**
`src/runtime/LocalizationService.cpp`, `src/runtime/VanillaKeys.cpp`, `src/runtime/Ids.h`,
`src/api/PapyrusApi.cpp`, `src/input/InputRouter.cpp`, `src/composite/D3D12Compositor.cpp`
Add `TrimAscii` (trim the same whitespace set as `std::isspace`), `ToLowerAscii`, and promote Ids'
`EqualsCaseInsensitiveAscii` into a shared header; migrate VanillaKeys/InputRouter/PapyrusApi/
LocalizationService (all ASCII inputs). Keep sites that need an owning/mutable string constructing
`std::string` from the view. **Do NOT delete** `D3D12Compositor::ModuleFileNameLower` — only
delegate its inner lowercasing loop (see §7).

### 4b.2 — `Version.h` targetVersion helpers — dedupe the newer-than-host check (3 sites) · **M / medium**
`src/runtime/ViewManifest.cpp`, `src/runtime/SettingsStore.cpp`, `src/runtime/Runtime.cpp`, `src/core/Version.h`
Add `IsTargetNewerThanHost(string_view)` (parse + `kPluginVersionParts < parts`) to replace
Runtime's flipped-operand `outdated` lambda, and an `AcceptTargetVersion(raw, logContext)` that
**preserves the raw author string** (never canonicalize — the badge/view-listing/diagnostics
payloads render it verbatim) and emits the per-site WARN. Merges the two near-verbatim
ViewManifest/SettingsStore blocks and the third open-coded copy.

### 4b.3 — `Json::CheckFormatVersion` for the 'declares newer' INFO block (4 sites) · **S / medium**
`src/runtime/VanillaKeys.cpp`, `src/runtime/ViewManifest.cpp`, `src/core/Config.cpp`
Add `Json::CheckFormatVersion(obj, key, known, source)` beside `Json::ReportUnknownKeys` doing
`GetInt`+compare+INFO once. Accept that wording unifies (adds the `(this build knows N)` clause to
ViewManifest, flattens Config's 'written by' nuance) — INFO-only, harmless.

### 4b.4 — `Json::GetStringArray` for the hand-rolled string-array extraction (4 sites) · **S / medium**
`src/core/Config.cpp`, `src/runtime/VanillaKeys.cpp`, `src/runtime/SettingsStore.cpp`, `src/runtime/Json.h`
Add `GetStringArray` (skip non-strings, empty when missing/not-array). `Config.views` collapses to
one line; VanillaKeys suppress and both SettingsStore alias loops keep their bodies but drop the
`is_array`/`is_string` guard. **Do NOT touch `Runtime.cpp:2250-2267`** (see §7 — it intentionally
coerces non-string args).

### 4b.5 — `Ids::ModOf` / `ViewNameOf` for the qualified-id split (5 sites) · **S / medium**
`src/runtime/Runtime.cpp`, `src/runtime/Ids.h`
Add `ModOf`/`ViewNameOf` to `Ids.h` next to the validators. Note the three view-name sites
concatenate into a `std::string`, so either wrap with `std::string(...)` or have `ViewNameOf`
return `std::string` while `ModOf` returns `string_view`.

### 4b.6 — Derive `KeyName` from the single `kNamedKeys` table · **M / medium**
`src/input/InputRouter.cpp`, `tests/native/hotkey_service_tests.cpp`
Promote `kNamedKeys` to a file-anonymous `constexpr` table and derive `KeyName` as "first entry
whose `.vk` matches" (first spelling per VK is canonical — verified to match every switch return).
Keep the F/letter/digit computed prefixes and the round-trip test as a regression guard.

---

## 4c. Theme C — Collapse copy-pasted sequences within native subsystems

*A fixed multi-line sequence is hand-inlined N times inside one subsystem, so copies must be kept
in lockstep by hand. Several refined proposals fix an unsafe detail the original glossed over.*

### 4c.1 — `ReloadViewInPlace` helper for the reset+LoadView+Resize triple · **M / medium**
`src/runtime/Runtime.cpp`
Add `ReloadViewInPlace(id, m)` and call from `DriveRecovery` and `DrivePendingOpen` retry directly;
for `DriveDevReload` call it then place `BroadcastViewsData` **after** (moving it before would
broadcast stale 'loaded' state). Keep call-site extras (`_recovery.erase`, trailing
`SendRuntimeReady`). Exclude `LoadSurface` (conditional Resize).

### 4c.2 — `IsViewReady` helper for the readiness predicate (2 sites) · **S / low**
`src/runtime/Runtime.cpp`
Wrap the `readySignal ? _readyViews.contains : Finished` predicate in a const helper called from
`BeginSurfaceOpen` and `DrivePendingOpen`.

### 4c.3 — `CollectConflicts` helper for the key-conflict loop (2 sites) · **M / medium**
`src/runtime/SettingsStore.cpp`
Extract the vk-match/self-exclude/`@game`-filter/`{mod,key,title}`-push loop into a static helper
taking `selfBlocksGameplay` as a **parameter** (the two callers derive it differently). Keep
`ConflictsFor`'s `vk==0` early return and `DataView`'s non-empty guard at the call sites.

### 4c.4 — `SettingsMirror` `LookupMod` / `LookupKey` helpers (4 fallback loops) · **M / medium**
`src/api/SettingsMirror.cpp`
Factor exact-then-CI lookup into two helpers, **but** keep `Find`'s up-front null-check on both
pointers **before** any `string_view` construction, and keep `ResolveNames`' empty-key branch a
caller-side guard (empty key resolves the mod only, without calling `LookupKey`). Add/keep unit
tests for empty-key whole-mod-resolve and exact-beats-case-variant.

### 4c.5 — Pipe message-builder helpers (8 host-message shapes authored twice) · **M / medium**
`src/render/WebView2HostWebRenderer.cpp`
Add JSON-builder helpers taking **primitive value args only** (`NavigateMsg(id,entry,bridge,
logicalHeight)`, `SetHiddenMsg(id,hidden)`, etc.) — **NOT** a `ViewRec&` (that would race/dangle if
a setter dereferenced it outside `stateMutex`). Snapshot loop passes `ViewRec` fields under the
lock; setters pass their params after the lock. Keep all send-gating and state side-effects in the
callers. (See §7 for why the `ViewRec&` form is unsafe.)

### 4c.6 — `PatchVtableSlot` helper for the `VirtualProtect` slot-swap (3 sites) · **M / medium**
`src/composite/D3D12Compositor.cpp`, `src/composite/UiPassSeam.cpp`
Extract a helper scoped strictly to the protect/write/protect dance (page-protection flag a
parameter), **never** to sourcing the chaining pointer. Callers keep their existing
capture-and-publish of the original **before** the call (store-before-write ordering is
load-bearing — the slot may dispatch on the render/FG thread mid-install). (See §7.)

### 4c.7 — `DispatchOne` helper for the Papyrus static-vs-method dyad (3 sites) · **S / medium**
`src/api/PapyrusApi.cpp`
Add `DispatchOne(vm, target, args)` taking the erased VM functor type by const-ref (both
`MakeArgs`/`MakeArgsArray` convert to it). `DispatchToTargets` and both `DispatchAction` branches
call it; keep the per-target closure construction (captures per-call `BSFixedString`s).

### 4c.8 — `ValidateActionModId` + collapse the eight `Register*` natives · **M / low**
`src/api/PapyrusApi.cpp`
Add `ValidateActionModId` (fold+`IsAcceptedModId`) shared by the four action registrars, and
collapse each Args/non-Args pair to one body plus the `wantsArgs` bool. Keep the distinct per-Kind
rules (`kSettings` raw, `kHotkey` non-empty+key) out of the action helper; the instance/static
emptiness check stays two tiny helpers (`BSTSmartPointer` vs `BSFixedString`).

### 4c.9 — Share the subscription-registry plumbing (Hotkey/Settings) · **M / medium**
`src/api/HotkeySubscriptions.cpp`, `src/api/SettingsSubscriptions.cpp`
Pull **only the provably-identical** plumbing (members, token-mint loop, `Unsubscribe`, and a
protected `InvokeLive` that owns the unlocked per-call liveness-rechecking tail) into a shared base.
Leave `OnFired`/`OnChanged` and each `Pump` body explicit — Settings' state-mutating replay pass and
`OnChanged`'s outside-the-lock JSON serialization have no Hotkey counterpart. **Do not** attempt a
full Pump-skeleton unification.

### 4c.10 — `EdgeLatch` helper for FreeCursor / SimPause edge latches · **S / low**
`src/input/FreeCursor.cpp`, `src/input/SimPause.cpp`
Give each site its own `EdgeLatch` instance; the helper must take an explicit `tryAcquire` step so
it can reproduce "null singleton → no-op, no flip, warn-once on acquire edge, retry next tick". Keep
FreeCursor's guarded decrement and live-refcount log in its lambda. If acquire-parameterization
makes the helper as long as the ~6–8 shared lines, **leave both functions as-is.**

### 4c.11 — `D3D12Prologue.h` for the GDI-free include prologue (3 TUs) · **S / low**
`src/composite/D3D12Compositor.cpp`, `src/composite/EngineD3D12.cpp`, `src/composite/UiPassSeam.cpp`
Move the `WIN32_LEAN_AND_MEAN`/`NOGDI`/`NOMINMAX` defines + `<Windows.h>`/`<d3d12.h>` + rationale
into a dedicated internal header, included at the same point in each TU. **Do NOT** fold into
`platform/WindowsPlatform.h` (a deliberately Win32-free facade used by many other consumers).

---

## 5. Theme D — Frontend duplication and React boilerplate

*Two views (settings, keybinds) and the UI kit each re-implement the same state-mirroring, capture,
and small render idioms by hand — several are byte-identical copies of a subtle, drift-prone
pattern.*

### 5.1 — `useStateRef` hook — the state+ref+dual-write triple (~8 sites across two views) · **M / medium**
`frontend/src/views/osfui/settings/App.tsx`, `frontend/src/views/osfui/keybinds/App.tsx`,
`frontend/src/views/osfui/settings/useCapture.ts`
Add `useStateRef<T>(init)` (`useState`+`useRef`+ref-then-state setter) and `useLatest` for
render-synced mirrors. Replace the five settings triples and three keybinds triples; sites that wrap
the pattern in extra logic (settings `applySave`, `useCapture`) build on top rather than becoming
one-liners. Move the "refs exist because once-registered bridge closures read first-render state"
rationale onto the hook doc.

### 5.2 — Reuse `useCapture` in keybinds (deletes ~90 lines of a duplicated race-sensitive state machine) · **M / high**
`frontend/src/views/osfui/keybinds/App.tsx`, `frontend/src/views/osfui/settings/useCapture.ts`
Generalize `CaptureTarget` to an opaque instance token (keybinds keys by `instanceId`, settings by
`mod:key`) and expose `capturingId`. Make the conflict-toast i18n address a hook option so keybinds
**keeps** `alsoBoundBy` (settings keeps `capturedAlsoBound` — naive reuse silently orphans the
catalog entry). Route **all** post-commit work (`setMods` + `settings.set` + `rebindRejected`/`get`
fallback + `setSelectedKey` + `setLoaded`) through `onCommit`, not just `setMods`. Verify the
keybinds catalog still resolves `alsoBoundBy`.

### 5.3 — `seedBaseline` helper (3 copies) · **M / medium**
`frontend/src/views/osfui/settings/App.tsx`
Extract `seedBaseline(base, modId, keys, values)` returning the new baseline or null. **Do NOT** fold
`captureBaseline`'s force-empty-entry rule into the shared changed-detection universally (`applyLocal`
can pass empty keys via `applyPreset` and would gain an extra render) — scope it to `captureBaseline`
via an `ensureEntry` flag or a `next[mod.id] ||= {}` in `captureBaseline` itself.

### 5.4 — `patchModValues` helper (3 copies) · **S / low**
`frontend/src/views/osfui/settings/App.tsx`
Extract `patchModValues(list, modId, patch)` for the
`m.id===modId ? {...m, values:{...(m.values||{}), ...patch}} : m` map; single-key sites pass
`{[key]:value}`. Natural home in `@lib/settings/modified`.

### 5.5 — `useCommittedText` hook — controlled-text re-seed (TextField/ColorField) · **S / medium**
`frontend/src/ui/TextField.tsx`, `frontend/src/ui/ColorField.tsx`
Extract `useCommittedText(committed): [text, setText]` owning the `useState`+`useRef`+`useEffect`
re-seed guard. Each component keeps computing its own `committed` (`value ?? ''` vs `value || ''`)
and its own apply/preset logic.

### 5.6 — `HudCard` should reuse the shared `Mark` component · **S / low**
`frontend/src/views/osfui/settings/Home.tsx`, `frontend/src/views/osfui/settings/marks.tsx`
Replace `HudCard`'s hand-rolled `iconFailed`/`showIcon`/`img-onError` chip with
`<Mark class="home-hud-chip" iconClass="home-hud-chip--icon" .../>`; add `Mark` to the `./marks`
import. Leave `MenuCard`'s Patch/monogram variant (fallback nests in an SVG frame).

### 5.7 — Shared `BrandEmblem` component (duplicated header SVG) · **S / low**
`frontend/src/views/osfui/keybinds/App.tsx`, `frontend/src/views/osfui/settings/App.tsx`
Extract the byte-identical emblem SVG (and optionally the wordmark) into `src/ui/` imported via
`@ui` in both headers; keep the per-view eyebrow as a prop. **Do NOT** wrap the whole `.brand` div
(settings' `.brand` also holds a version-stack sibling keybinds lacks).

### 5.8 — Single owner for `HOME_ID` · **S / medium**
`frontend/src/lib/lifecycle.ts`, `frontend/src/lib/settings/rail.ts`
Create a small leaf `frontend/src/lib/ids.ts` exporting `HOME_ID`; both import it and `rail.ts`
re-exports so existing consumers keep working. Avoid making `lifecycle.ts` (a deliberately
dependency-light state module) import the heavier `rail` module.

### 5.9 — `interpolateEnglish` helper (`windowBridge.t` / `nullBridge.t`) · **S / medium**
`frontend/src/lib/bridge.ts`
Extract one module-local `interpolateEnglish(english, vars)` and call from both `t` implementations
— the two copies are byte-identical.

### 5.10 — `isModified` should reuse `sameValue` · **S / low**
`frontend/src/lib/settings/modified.ts`
After the two guards, replace the inline object/scalar compare with
`return !sameValue(value, setting.default);` — `sameValue` already encapsulates the flags-array trap
and is reused in `sessionDiff`.

### 5.11 — Share `OPEN_COOLDOWN_MS` · **S / low**
`frontend/src/views/osfui/settings/Detail.tsx`, `frontend/src/views/osfui/settings/Home.tsx`
Move the `1600`ms constant to a shared settings module imported by both; keep the two call sites'
distinct disabled/label semantics (optionally a `useOpenCooldown` hook that preserves them).

### 5.12 — Single `WORKLOADS` lookup helper · **S / low**
`frontend/src/views/osfui/benchmark/App.tsx`
Add `workloadOf(id)` and read `w?.name ?? id` / `w?.detail` in the caption instead of two adjacent
finds; keep `workloadName` as a thin wrapper for the bare-string use in `finishRun`.

### 5.13 — `TimedSummary` type + reuse `inputP95` · **S / low**
`frontend/src/views/osfui/benchmark/App.tsx`
Introduce `TimedSummary extends FrameSummary { timerP95 }` shared by `LiveResult` and
`BenchmarkResult`, and reuse the already-computed `inputP95` in `copyResults` instead of recomputing
percentile.

### 5.14 — `ViewRowText` fragment (Detail.tsx, 3 sites) · **S / low**
`frontend/src/views/osfui/settings/Detail.tsx`
Extract `ViewRowText({view})` for the identical `.row-text` sub-tree in `PanelRow`'s two branches
and `HudRow`. Exclude the different-shaped `ActionRow` block (~line `441`) and Home cards.

### 5.15 — `cx` / `optAttr` kit helpers for class-concat and conditional-attribute idioms · **S / low**
`frontend/src/ui/Badge.tsx`, `frontend/src/ui/Row.tsx`, `frontend/src/ui/Note.tsx`, `frontend/src/ui/KeyField.tsx`
Add variadic `cx(...parts)` (falsy-skip, joins in argument order) and `optAttr(name, value)`
(omit-when-falsy, accepts non-string values). **Preserve class ORDER** — KeyField's `.listening` and
ActionButton's `.pending` are padnav selectors that must stay last. Consolidate the
`exactOptionalPropertyTypes` rationale into the helper doc.

> **Correction (post-implementation).** The stated reason for preserving order is wrong: padnav's
> `.listening` read is a PRESENCE test (`padnav.js:184`) and it never queries `.pending` at all, so
> neither is an order-sensitive selector — class order inside a `class` attribute is invisible to
> CSS and `querySelector` alike. Preserving the order is still right, but as a kit convention pinned
> verbatim by `dom-contracts.test.tsx`, not as a padnav contract. See `SIMPLIFICATION_PLAN.md` §5b.

---

## 6. Theme E — Delete dead code and vestigial machinery

*Standing machinery whose consumer, kill-switch, or investigation is gone. All grep-confirmed
unreached and deletable with no behavior change.*

### 6.1 — Delete the dead dirty-rectangle machinery · **S / medium**
`src/render/IWebRenderer.h`, `src/render/WebView2HostWebRenderer.cpp`
Remove `DirtyRect::Union`/`Empty`, `FrameBufferView::dirty` and its single `Full()` assignment
(~line `1233`) — never read; every frame is already fully dirty.

### 6.2 — Delete dead `MenuController::ToggleDefault` · **S / low**
`src/runtime/MenuController.cpp`, `src/runtime/MenuController.h`
Remove the method (zero callers; the live toggle lives inline in `ApplyMenuRequests`). Leave the
unrelated `MenuReq::ToggleDefault` enum.

### 6.3 — Remove the `g_creatorReady` dead kill-switch · **S / low**
`src/input/FocusMenu.cpp`, `src/input/FocusMenu.h`
Delete the variable (always true) and its unreachable guard in `Register()`; update the
`FocusMenu.h` doc reference so no dangling mention remains.

### 6.4 — Delete vestigial `[wheel-probe]` logging · **S / low**
`src/input/OverlayInputHook.cpp`
Remove both devMode-gated wheel-probe INFO blocks (and the `HIWORD` reinterpret that only fed a
discarded log). Routing and legacy-message swallow are untouched.

### 6.5 — Strip EngineInput's dead characterization counters + ring · **M / medium**
`src/input/EngineInput.cpp`
Remove the nine atomic counters, the `ButtonRecord` ring + mutex, and their writes from the thunks.
Keep `Thunk_ShouldHandleEvent` returning true and slots 5/6 as EMPTY no-op thunks (do **not** restore
engine handlers). Reduce `LogSessionSummary` to the routing-state reset (rename to
`ResetSessionRouting`).

### 6.6 — Delete the orphaned `LoadLibrary` doc comment · **S / low**
`src/platform/WindowsPlatform.h`
Remove the comment block (lines `11–15`) documenting a function that no longer exists.

### 6.7 — Delete `rail.ts`'s dead `cycleRail` · **S / medium**
`frontend/src/lib/settings/rail.ts`, `frontend/test`
Delete `cycleRail` (production uses `railNodes` + `lifecycle.ts`'s `cycleRail`) and its test
block/import; keep `sortedMods`. Optionally reword the stale doc references in `rail.ts` and
`Rail.tsx`.

### 6.8 — Remove vestigial `data-label` / `data-mod` plumbing · **M / low**
`frontend/src/ui/Row.tsx`, `frontend/src/views/osfui/settings/SettingRow.tsx`,
`frontend/src/views/osfui/settings/Detail.tsx`, `frontend/src/views/osfui/settings/Rail.tsx`
Drop the `dataLabel` prop, `labelText()`, and Rail's `data-mod` (only a self-fulfilling contract
test reads them; grep confirms no CSS/padnav/JS consumer). Remove the test assertion. **Keep
`dataKey`** (live search-jump anchor). Touch **every** `dataLabel` call site together or the strict
TS build fails.

### 6.9 — Retire the deprecated `execCommand` clipboard fallback · **S / low**
`frontend/src/views/osfui/benchmark/App.tsx`
Remove the hidden-textarea + `execCommand` branch but **KEEP** a single try/catch: `writeText` →
'Copied', catch → 'Copy failed'. Do **not** drop error handling entirely (`writeText` can reject on
focus loss in this overlay).

### 6.10 — Model `MenuController`'s single-menu policy as `std::optional` · **M / low**
`src/runtime/MenuController.h`, `src/runtime/MenuController.cpp`
Replace the vector-as-stack (only ever 0/1 element) with `std::optional<std::string>`;
`DesiredLayers` gives the active menu a fixed `z=1000`. Refresh the stack-flavored vocabulary in
comments/docstrings.

---

## 6b. Theme F — Clarity: redundant state, misleading names, needless indirection

*A few spots present more machinery than the behavior warrants. Some need a specific mechanism fix.*

### 6b.1 — Delete the `InputRouter` class and inline routing into `OnHostKey` · **M / medium**
`src/input/InputRouter.cpp`, `src/runtime/Runtime.cpp`, `tests/native/run.sh`
Inline the ~15-line routing (which already re-derives capture/toggle that `OnHostKey` computes) into
`Runtime::OnHostKey`, keeping the invalid-toggle-key guard. **Do NOT** delete `InputRouter.cpp`
wholesale — lines ~`9–137` are `ResolveKeyName`/`KeyName` used across many subsystems and
unit-tested; keep them (rename the file to `KeyNames.cpp` and repoint `run.sh`).

### 6b.2 — Remove the `g_seamDrawFn` atomic + `SeamDrawThunk` double-hop · **M / medium**
`src/composite/D3D12Compositor.cpp`, `src/composite/D3D12Compositor.h`
Have `RecordSeamOverlayDraw` load `g_overlay` directly with acquire ordering, deleting the atomic +
thunk. This requires a `friend` declaration for the free function (`Impl` is a private nested type —
the original "same TU can name `Impl`" premise is false and will not compile). Preserve the
null-check gating.

### 6b.3 — Rename `SetSeamDrawMode`/`seamMode` → `SetSeamHooked` (C++ symbols only) · **M / low**
`src/composite/ICompositor.h`, `src/composite/D3D12Compositor.cpp`, `src/composite/D3D12Compositor.h`
Rename the method/atomic/stats field and fix the header comment (it publishes seam-install status,
not a draw mode). Keep the false-on-Install-failure signal. **Leave the wire-facing JSON key
`seamMode` and `drawPath` `'ui-seam'`/`'present'` literals unchanged** (bridge-protocol contract —
see §7).

### 6b.4 — Collapse the focus watchdog to one episode flag · **S / low**
`src/render/WebView2HostWebRenderer.cpp`
Replace `focusFixWarned` + `focusStrandReported` with one `inStrandEpisode`, compute
`stranded = !healthy`, and drive both effects off it. **KEEP** each WARN inside its own branch with
its distinct message (gate on `!inStrandEpisode`); write `inStrandEpisode` **only** at the report
edge, never inside a branch.

### 6b.5 — MessageBridge: drop the redundant trailing reset + inline `EncodeMessage` · **S / low**
`src/runtime/MessageBridge.cpp`, `src/runtime/MessageBridge.h`
Remove the trailing in-flight reset (entry-time reset is authoritative) and update the
`CurrentRequestId` doc comment; inline `EncodeMessage` at its single call site. **Do NOT** touch the
entry reset (load-bearing for malformed/unknown-type paths).

### 6b.6 — Simplify the diagnostics `Snapshot` sort to pointer-sort · **S / low**
`src/runtime/DiagnosticsModule.cpp`
Sort `std::vector<const Issue*>` and use an explicit multi-key comparator (tuple) instead of the
index vector + packed `OrderRank`. **CRITICAL:** keep an explicit DESCENDING insertion tiebreak
(newest-first) — do **not** rely on `stable_sort` stability, which yields oldest-first and would
silently reverse same-tick ordering. Add a same-rank/same-timestamp ordering test.

---

## 7. Do-NOT-touch (looks simplifiable, but load-bearing — rejected on verification)

These were flagged by analyzers and **rejected** because the complexity is intentional. Removing or
naively "simplifying" them regresses known crashes or breaks wire contracts.

1. **`IWebRenderer`'s ~30 no-op default virtuals** — they keep Runtime's game-thread call path
   branch-free across the real WebView2 backend, the Null fallback, and the Mock test double with
   NO capability checks or `dynamic_cast`. Several defaults absorb `this`-capturing callbacks under
   documented thread-affinity contracts. A "narrower facet" relocates complexity into every call
   site for a self-described modest gain.

2. **`CompositorStats::busyWaits`/`droppedBusy` zero-valued wire fields** — deliberately retained
   (comment + `docs/seam-draw-design.md`) so the host diagnostics page needs no version dance.
   Removal is a coordinated native+wire+host/frontend protocol change, only free once such a bump is
   already happening.

3. **`NormalizeLocale`'s BCP-47 segment-casing loop** — pinned by `localization_service_tests.cpp`
   (`zh_hans` → `zh-Hans`) and it forms catalog keys from file stems. The "simpler"
   lowercase-lang/uppercase-rest rule yields `zh-HANS` and silently breaks catalog resolution for a
   `t.mod_zh-hans.json` file.

4. **Runtime's gamepad `DrainEngineInput`** — do **NOT** extract to a `GamepadRouter`. It is woven
   into load-bearing input ordering (EngineInput consume/raw mode, focus-gated XInput baseline
   suppression so the opening button can't leak as an activation, `EnqueueMenuRequest` marshalling,
   WndProc-thread cursor atomics). Only the **diagnostics** half of the Runtime split (§4a.1) is safe.

5. **`D3D12Compositor::ModuleFileNameLower`** — do **NOT** fold into the shared `ToLowerAscii`. It
   does basename extraction before the `sl.dlss_g` prefix match that drives DLSS-G FrameGen
   detection. Lowercasing a full path would leave `dlssFg` permanently false and silently regress the
   FrameGen CTD (reports #2/#4) draw-suspension mitigation. Only delegate its inner lowercasing loop.

6. **`Runtime.cpp` args handling (~2250–2267)** — do **NOT** apply `Json::GetStringArray` here. It
   intentionally COERCES non-string elements (protocol 1.3: `args:[1,7]`) and falls back to the
   scalar `arg` key; the skip-non-strings helper would silently drop numeric/bool args and lose the
   fallback.

7. **`ThreadAffinityProbe`** — do **NOT** delete now. `NoteRuntimeTick` validates the just-landed
   (~1 day old) "Run Runtime tick on main thread" fix that is likely not yet in-game-confirmed, and
   it guards the load-bearing thread-affinity / `MainThreadMenuPump` subsystem where IDs/threads
   silently re-bind across game patches. Follow `uiPassProbe`'s keep-then-remove cadence (demote
   after in-game confirmation, delete after a release of stability).

8. **Wire-facing strings `seamMode` (JSON key) and `drawPath` `'ui-seam'`/`'present'`** — in the
   `SetSeamDrawMode` cleanup (§6b.3), rename only the C++ symbols. These literals are bridge-protocol
   contract consumed by the frontend diagnostics fixtures and would require a versioned protocol
   change.

9. **`FocusMenu`'s unused `kFlag` constants** — if trimmed, relocate the FULL per-flag RE provenance
   (kModal suppresses 3D render and the input gate is bit 4 not bit 8; the freeze-latch's live-proven
   CLSF `ID::IMenu::Unk0E{130622}`, the "latch-on-non-modal is unnatural, soak before shipping"
   warning, and that real pause is via `UI::ModifyMenuPauseCounter`) to the `.cpp` write site. Do
   **NOT** collapse into the terse header bit-number comment — that discards hard-won reversing
   detail.

10. **`ReconcileList`'s liveness/count gates — KEEP; the `kListStableTicks=3` debounce — VALIDATE-THEN-REMOVE.**
    ⚠ *Revised after git verification.* The original claim bundled these as one indivisible report-#3
    mitigation; that was wrong. `git log -S 'kListStableTicks'` shows the debounce was added in a
    single commit (`426146a`) *alongside* the `MainThreadMenuPump` fix — whose own message says
    *"timing debounces could never fix it"* (the CTD was a cross-thread race the pump fixed). The
    liveness/count gates (`LivePauseMenu`, `count<=0`, `count==expectedCount`) **predate** the
    debounce (`029d850`, ran with no debounce) and remain **load-bearing** — keep them exactly. The
    `kListStableTicks`/`stableTicks` debounce is the one piece never proven necessary under the
    corrected boundary: **test-remove it** behind a mandatory FSR3-Frame-Generation-ON in-game
    acceptance run (report #3 was FG-specific). The §4a.4 extraction preserves the gates; the
    debounce disposition is settled first (plan Phase 2). Also fix the incorrect CTD attribution in
    the `PauseMenuEntry.cpp` comment.

11. **`padnav.js` `focusFirst()`** — deletion is behavior-safe (zero callers) but padnav is a
    deliberately frozen compatibility boundary (`COMPATIBILITY.md §3`) that jsdom cannot validate;
    fold the removal into the sanctioned padnav conversion pass gated on in-game controller
    verification rather than an ad-hoc dead-code sweep.

12. **Pipe message-builder helpers must take primitive value args, not a `ViewRec&`** (the original
    `NavigateMsg(const ViewRec&)` form): setter `Send`s run OUTSIDE `stateMutex` and read params by
    design, and the `views` vector reallocates — a `ViewRec` deref there is a new data race the
    current code deliberately avoids. (Constrains §4c.5.)

13. **The vtable `PatchVtableSlot` helper must not source the chaining pointer from its return
    value** — each site publishes the original BEFORE overwriting the slot (store-before-write)
    because the slot may dispatch on the render/FG-pacing thread mid-install; a
    returned-old-value-as-chain would open a null-original window. (Constrains §4c.6.)

---

## 8. Provenance

Produced by a 75-agent orchestrated audit (14 subsystem analyzers → adversarial per-finding
verification → synthesis) over commit `e4db141`. Verifiers consulted the source, `CHANGELOG.md`,
`docs/`, and git history to separate accidental from load-bearing complexity. Numbers: 60 raw
findings → 27 confirm, 30 refine, 3 rejected. Line references are approximate and should be
re-anchored by symbol if HEAD has advanced.
