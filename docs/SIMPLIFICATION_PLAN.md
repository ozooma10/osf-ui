# OSF UI — Simplification Execution Plan (fused)

> **What this is:** the reconciled, sequenced execution plan that fuses **three** independent
> audits into one, with every new/conflicting claim verified against this repo at HEAD.
> It is the companion to the detailed finding catalog in
> [SIMPLIFICATION_AUDIT.md](./SIMPLIFICATION_AUDIT.md) — this doc **references** that one's
> section numbers (§4a.1 etc.) rather than restating every item.
>
> **Generated:** 2026-07-24 · **Anchored to:** `e4db141` (HEAD).
> **Sources fused:**
> - **Audit A** — `SIMPLIFICATION_AUDIT.md`: 57-item duplication/dead-code catalog. *Scope: only
>   `src/` and `frontend/` — it never looked at `tools/webview2_host/`, `docs/`, or protocol names.*
> - **Audit B (Codex)** — a concentrated-responsibility (god-object) review. *Ran on a different
>   macOS checkout, so its line numbers were treated as claims and re-anchored here by symbol.*
> - **Audit C (thread-retirement)** — which defensive complexity became redundant after the runtime
>   tick was correctly moved to the game main thread.
> - **Verification pass** — 6 agents that checked B's and C's disputed claims against HEAD + git
>   history, plus a fusion agent.

---

## 1. Reconciliation summary

The three audits are **largely complementary, not contradictory**, once scope is accounted for:

- **A** is the most concrete and directly actionable, but scoped only to `src/` + `frontend/`.
- **B (Codex)** correctly reframes the core problem as **concentrated responsibility** (god objects).
  Its strongest example is one A literally could not see: **`tools/webview2_host/HostApp.cpp` is a
  2935-line file that is one anonymous `struct App`** (~2685 lines, ~60 methods) fusing every host
  concern — the single largest concentrated-responsibility unit in the tree.
- **C** is correct that defensive complexity was built on the now-falsified belief that SFSE tasks
  ran on the main thread, and that the whole-tick move (`7ab68ad`, +220/−398) retired ~398 lines of
  marshalling latches. Verification confirmed C's `426146a → 78bd3e0 → 7ab68ad` sequence, with one
  precision fix: there are **two distinct main-thread boundaries** — the reconcile-specific one
  (`426146a`, `MainThreadMenuPump`, where the pause-menu debounce lives) and the whole-tick one
  (`7ab68ad`).

**Only two genuine cross-audit conflicts existed** (pause-menu debounce; IWebRenderer surface) —
both resolved below into compatible subsets.

### Codex claims — what verified vs. did not (HEAD `e4db141`)

**Verified true:** the concentrated-responsibility thesis; the ~2700-line HostApp figure (actual
2935 file / ~2685 struct); broken doc links (confirmed and *broader* — five missing docs, not two);
protocol-constant duplication; IUiModule's factual sub-claims (2 impls, 38 concrete reach-throughs,
settings-specific `BuildModules`, empty `DiagnosticsModule::OnStart`); IWebRenderer's oversized
surface (381 lines, ~46% pure type declarations).

**Did NOT hold as stated:** "remove IUiModule" (refuted — it earns a keep as an ordered lifecycle
fan-out; only the comments are wrong); "shrink IWebRenderer" is only *partly* compatible with A's
keep-the-virtuals rule (type extraction + a guarded handler struct are safe; a queryable
`RendererInput`/`dynamic_cast` facet is not); the claim that `src/runtime/Ids.h` is a partial home
for protocol names (inaccurate — `Ids.h` is id-grammar validation, not message-name constants).

---

## 2. Resolved tensions (the evidence)

### 2.1 Pause-menu `kListStableTicks=3` debounce — **A said keep, C said test-remove → C is right**
A's do-not-touch #10 bundled the debounce **and** the liveness/count gates as one indivisible
"report #3 mitigation." That bundling is the error. `git log -S 'kListStableTicks'` shows **exactly
one commit** (`426146a`), which added *both* `MainThreadMenuPump` and the debounce — and whose body
states *"timing debounces could never fix it"* (the CTD was a cross-thread race the pump fixed). The
liveness/count gates (`LivePauseMenu`, `count<=0`, `count==expectedCount`) **predate** the debounce
(they arrived in `029d850`, which ran with **no** debounce). Therefore:
- **Keep** the liveness/count gates exactly — load-bearing.
- **Test-remove** only `stableTicks`/`kListStableTicks` (+ the `g_session.lastSeenCount`/`stableTicks`
  fields), gated on a mandatory **FSR3-Frame-Generation-ON** in-game acceptance run (report #3 was
  FG-specific). → Phase 2.

### 2.2 IWebRenderer surface — **~80% compatible; one seam is the collision**
Codex mostly attacks file **girth**, not the polymorphism A protects.
- **Adopt:** extract the value types at `IWebRenderer.h:5-179` into `src/render/RenderTypes.h`
  (~46% of the file, zero polymorphism change).
- **Optional, guarded:** collapse the 7 *set-once global* handler setters into one
  `SetHandlers(const RendererHandlers&)` keeping **one** no-op default (branch-free preserved),
  carrying each callback's non-uniform thread-affinity comment per-field; exclude per-view
  Console/JS primitives (set dynamically).
- **Reject:** any queryable `RendererInput` facet via `AsInput()`/`dynamic_cast` — that reintroduces
  exactly the null-branch A §7.1 forbids. Preserve `InjectPhysicalMouseWheel`'s *real delegating*
  default (not a no-op).

### 2.3 IUiModule — **Codex "remove" refuted → keep, fix the comments**
Codex's factual bill is all true, but remove-or-make-real is a false choice. The interface earns a
keep as a **uniform lifecycle fan-out**: four live polymorphic loops (`OnStart`, `RegisterCommands`,
`OnBridgeDown`, `OnViewDestroyed`) over an ordered heterogeneous vector; `OnViewDestroyed`/
`OnBridgeDown` scrub per-view `_subscribers` or pushes leak for process lifetime; `_diagnostics`
must be constructed before `SettingsModule::OnStart`. The only defect is **comment honesty**
(`UiModule.h:7-11`, `Runtime.h:129`, `SettingsModule.h:12-13` falsely claim the core is
module-agnostic — 38 concrete reach-throughs prove otherwise). → Phase 1 doc-comment fix.

### 2.4 `LogSessionSummary` "just delete it" — **carries a behavioral routing reset**
The 9 counters + ring are diagnostic-only and removable, **but** `LogSessionSummary` also does a
behavioral gamepad routing reset (`EngineInput.cpp:296-306`, clears `g_padHead`/`g_padCount`, zeroes
sticks under `g_padMutex`) feeding `PollGamepadButton`/`GetSticks`. Extract `ResetSessionRouting()`
and keep calling it on the overlay-close edge (`Runtime.cpp:1698`); delete only the counters/ring/log.

### 2.5 Watchdogs — **two different animals**
The **WebView focus watchdog** (`WebView2HostWebRenderer.cpp:1145-1198`) heals an out-of-process
Chromium async `MoveFocus` race that is **independent** of where Tick runs → **KEEP unchanged**. The
**admitted-state watchdog** (`Runtime::ReconcileFocusMenu:1707-1740`) was created in the wrong-thread
era; plausibly idle after `7ab68ad` but **unproven** → **instrument, don't yet remove** (Phase 2).

### 2.6 Present-hook — **retired after three-way compatibility reproduction**
The slot-8 Present hook was removed on 2026-07-27 after reproducing a crash in the wrapped
OptiScaler/Streamline + Steam overlay chain. Output sizing and FG classification now come from
`UiPassSeam`, and shared-ring adoption runs on `Submit()`. Do not reintroduce swapchain probing or
Present chaining without a new design and the external-overlay compatibility matrix.

---

## 3. Phased execution plan

Each phase has a **gate** that must pass before moving on. Native tests: `bash tests/native/run.sh`
(exit code = failure count). Frontend: `npm --prefix frontend run verify` (typecheck + build +
vitest). Some items **require an in-game pass** and cannot be validated headless — those are isolated.

> ⚠ **Stale-code trap** for any in-game gate: build fresh and confirm the *enabled* MO2 mod is your
> `xmake` output, **not** `OSF UI DIST`, or the game runs old code and your test proves nothing.

### Phase 1 — Safe sweep (no engine assumptions, headless-verifiable)
**Goal:** land every behavior-free deletion, diagnostics-only strip, and stale-doc/comment fix. All
three lenses converge here. One reviewable commit per logical group.
**Gate:** native + frontend suites green. No in-game pass required. (For the `data-label` removal,
touch every call site in one commit or the strict TS build fails.)

| Item | Files | Source |
|---|---|---|
| Delete dead dirty-rectangle machinery (`DirtyRect::Union/Empty`, `FrameBufferView::dirty`) | `src/render/IWebRenderer.h`, `WebView2HostWebRenderer.cpp` | A §6.1 |
| Delete dead `MenuController::ToggleDefault` (keep the enum) | `src/runtime/MenuController.{cpp,h}` | A §6.2 |
| Remove `g_creatorReady` dead kill-switch + unreachable guard | `src/input/FocusMenu.{cpp,h}` | A §6.3 |
| Delete `[wheel-probe]` logging (keep the behavioral wheel block + legacy return) | `src/input/OverlayInputHook.cpp` | A §6.4 |
| Strip EngineInput counters+ring; **extract `ResetSessionRouting()`**, keep it on close edge | `src/input/EngineInput.cpp`, `Runtime.cpp:1698` | A §6.5 + verify |
| Remove `MenuEventSink::s_openMenus` counter + DevMode log (keep `s_consoleOpen`, CloseAll force-hide) | `src/input/MenuEventSink.{cpp,h}` | verify (new) |
| Delete orphaned `LoadLibrary` doc comment | `src/platform/WindowsPlatform.h` | A §6.6 |
| Delete `rail.ts` dead `cycleRail` + its test | `frontend/src/lib/settings/rail.ts` | A §6.7 |
| Byte-identical FE dedupe: `interpolateEnglish`, `isModified→sameValue`, shared `OPEN_COOLDOWN_MS` | `bridge.ts`, `modified.ts`, `Detail.tsx`, `Home.tsx` | A §5.9/5.10/5.11 |
| Rewrite `architecture.md` to the UiPassSeam draw path (Present hook draws nothing; delete mock/CPU upload path) | `docs/architecture.md` | verify |
| Restore/re-target 5 dangling design docs (`mcm-design.md` 29 refs, `api-freeze-plan.md` 21 refs, `form-references-design.md`, `reverse-engineering-notes.md`, `ROADMAP.md` link) | `docs/` | verify |
| Fix IUiModule doc-comments (stop claiming core is module-agnostic); drop redundant empty `DiagnosticsModule::OnStart` | `UiModule.h`, `Runtime.h`, `SettingsModule.h`, `DiagnosticsModule.h` | verify (Codex resolved) |
| Fix pause-menu debounce comment's wrong CTD attribution (`PauseMenuEntry.cpp:286-296`) | `src/input/PauseMenuEntry.cpp` | verify (C) |

### Phase 2 — Bounded thread-defense retirement experiments (in-game gated)
**Goal:** retire exactly the defenses verification shows became redundant after the main-thread move,
**one variable at a time.** Nothing here is safe headless.
**Gate (per experiment):** fresh build + correct MO2 mod, then an **FSR3-FG-ON** in-game run: 30+
pause open/close cycles, rapid mash, open during/after save-load and main-menu↔game transitions, open
with stacked menus. **Pass** = zero CTD (no NaN-atom deref at `PopulateMainList` / `+333AB2E`), "MOD
MENUS" entry present every open, no flicker/double-entry. Repeat with FG OFF. Add the named native
characterization test **before** touching runtime code.

| Item | Files | Source |
|---|---|---|
| **Test-remove** `kListStableTicks` debounce only (keep `LivePauseMenu`/count gates); add ReconcileList characterization test first; if any recurrence → debounce is validated, keep it | `src/input/PauseMenuEntry.cpp`, `tests/native` | C + verify |
| **Instrument** (do NOT yet remove) the admitted-state focus watchdog — add fired-count counter, ship, confirm zero fires; keep the WebView focus watchdog untouched | `src/runtime/Runtime.cpp` | verify (C) |

### Phase 3 — Missing-helper & duplication consolidation (A themes B/C/D)
**Goal:** fold A's reinvented primitives, copy-pasted native sequences, and frontend boilerplate onto
shared helpers, following each item's refined guardrails exactly. One helper per commit.
**Gate:** native + frontend green after each helper. The two race-sensitive FE items (§5.1 `useStateRef`,
§5.2 `useCapture` reuse) land isolated; §5.2 also needs an in-game controller smoke (keybinds catalog
still resolves `alsoBoundBy`). Protocol-name swap must keep emitted wire strings byte-identical
(`osfui.js` is byte-compared on build).

| Item | Source |
|---|---|
| `core/StringUtil.h`: `TrimAscii`/`ToLowerAscii`/`EqualsCaseInsensitiveAscii` — **LANDED** in `d22943b`; ~21 `StringUtil::` sites at HEAD, and the header now also carries the UTF-8 group (`Utf8TruncateLen`/`TruncateUtf8`/`SkipLeadingUtf8Continuations`). The `ModuleFileNameLower` caveat is void — that function is gone (see §4). | A §4b.1 |
| `Version.h` targetVersion helpers + `Json::CheckFormatVersion` + `Json::GetStringArray` (do NOT touch `Runtime.cpp:2250-2267`) | A §4b.2/4b.3/4b.4 |
| `Ids::ModOf`/`ViewNameOf` (5 sites) + derive `KeyName` from `kNamedKeys` | A §4b.5/4b.6 |
| Native copy-paste collapse §4c.1–4c.11, each with its guardrail (pipe builders take primitive args; `PatchVtableSlot` doesn't source the chain ptr; `D3D12Prologue.h` not folded into WindowsPlatform.h; etc.) | A §4c |
| Frontend duplication §5.1–5.15 + §6.8 (`data-label` removal, keep `dataKey`) + §6.9 (`execCommand`, keep one try/catch) | A §5/§6.8/§6.9 |
| **Centralize bridge command/event names** into `src/runtime/Protocol.h` + one TS module; test C++ set == TS set == SDK union; wire values byte-identical; `Ids.h` is NOT a home | verify (Codex) |

### Phase 4 — God-object decomposition: highest-payoff / lowest-risk first
**Goal:** pull self-contained collaborators out of the two biggest units (Runtime, HostApp.cpp) as
behavior-preserving code-motion, plus the settings-frontend controller split.
**Gate:** native + frontend green per extraction. RuntimeDiagnostics: diagnostics native tests pass.
HostApp.cpp extractions have **no headless harness** → each needs an in-game overlay smoke (open/close,
multi-view register, keyboard+mouse+gamepad, focus in/out); add characterization tests before
`FrameTransport` and `HostInputSession`. **If no host test harness can be stood up, stop the HostApp
sequence after the JS-as-asset step** rather than moving thread-sensitive code blind.

| Item | Files | Source |
|---|---|---|
| Extract `RuntimeDiagnostics` collaborator (3 forwarded edges; do NOT extract `DrainEngineInput`) — **lowest-risk highest-payoff Runtime pull** | `src/runtime/Runtime.{cpp,h}` | A §4a.1 + Codex |
| Split HostApp `HandleCommand` 185-line chain (`:2539-2723`) into per-type methods / dispatch table | `tools/webview2_host/HostApp.cpp` | verify (new) |
| Factor `AddDocumentScript` helper for the repeated AddScript boilerplate | `tools/webview2_host/HostApp.cpp` | verify (new) |
| Move ~300 lines of injected JS into build-embedded `.js` assets (compiled-in bytes only; preserve install order) — **highest-value HostApp win** | `HostApp.cpp`, `xmake.lua` | verify (new) |
| Extract `FrameTransport` (ring+fences+publish; carry `ringMutex` + capture/STA thread contract) | `tools/webview2_host/HostApp.cpp` | verify (new) |
| Extract `HostInputSession` (capture subclass, raw mouse, host wndprocs; keep `s_app` singleton) | `tools/webview2_host/HostApp.cpp` | verify (new) |
| Settings frontend → `useSettingsController` reducer + view; benchmark → `useBenchmark()` hook (after Phase 3 helpers) | `settings/App.tsx`, `benchmark/App.tsx` | Codex + A §4a.3 |
| Extract IWebRenderer value types → `RenderTypes.h` (~46% shrink, zero polymorphism change) | `src/render/IWebRenderer.h`, `RenderTypes.h` | verify + Codex + A §6.1 |
| Extract `OverlayValues` from `AddSchema`; lift JSON projection out of `SettingsStore` | `src/runtime/SettingsStore.cpp` | A §4a.2 + Codex |

### Phase 5 — Most-entangled extractions & CTD-adjacent decompositions (last)
**Goal:** the refactors that touch reveal-hide state machines, the pause-menu VM path, seam-draw
indirection, or the render/FG thread. Each: isolated reviewed commit + characterization tests and/or
in-game verification.
**Gate:** native + frontend green; pause-menu/seam/FG-thread changes additionally need the FSR3-FG-ON
smoke; `ViewCollection` needs host characterization tests (reveal/hide/prewarm/timeout) + in-game
multi-view smoke; verify the seam-only compositor across FG off, native FSR3 FG, and external frame-generation/overlay chains.

| Item | Files | Source |
|---|---|---|
| Extract HostApp `ViewCollection` / reveal-hide state machine (most entangled; do last) | `tools/webview2_host/HostApp.cpp` | verify (new) |
| Decompose 175-line `ReconcileList` (keep guards in parent; after Phase 2 settles the debounce) | `src/input/PauseMenuEntry.cpp` | A §4a.4 |
| Delete `InputRouter` class + inline routing; rename remainder → `KeyNames.cpp` | `InputRouter.cpp`, `Runtime.cpp`, `run.sh` | A §6b.1 |
| Remove `g_seamDrawFn` atomic + `SeamDrawThunk` double-hop (needs `friend` decl) | `src/composite/D3D12Compositor.{cpp,h}` | A §6b.2 |
| Remaining §6b clarity fixes (`SetSeamHooked` rename — symbols only; focus-watchdog one-flag; MessageBridge trailing reset; diagnostics pointer-sort) + optional IWebRenderer handler-struct collapse (no `RendererInput` facet) | multiple | A §6b + verify |

---

## 4. Consolidated do-not-touch (load-bearing across all three audits)

- **Thread affinity / `MainThreadMenuPump`** (byte-exact `UI_AdvanceActiveMenus` call-site hooks, IDs
  99438/130455, fail-closed) — *this*, not any debounce, fixed the report #3 CTD family.
- **`ReconcileList` liveness/count gates** — `LivePauseMenu()` teardown barrier, `count<=0`
  wait-for-engine-push, `count==expectedCount` steady-state skip. Separable from and independent of
  the debounce.
- **FrameGen / FSR3 / DLSS-G seam target selection** — ordinary UI target with FG off, transparent COPY_SOURCE handoff with FG on.
- **Universal `UiPassSeam` composition** — no probe swapchain and no slot-8 Present hook. Sizing, FG
  target selection, and ring adoption must remain independent of external DXGI hook chains.
- **IWebRenderer's ~28 no-op default virtuals** — keep the branch-free game-thread path; do NOT convert
  any optional method into a queried-capability + call-site branch. `SupportsMultipleViews()`/
  `UsesNativeKeyboardFocus()` are the only two intentional capability queries. `InjectPhysicalMouseWheel`'s
  default is a real delegating impl. Preserve the non-uniform thread-affinity comments.
- **`CompositorStats::busyWaits`/`droppedBusy`** zero-valued wire fields (host diagnostics needs no
  version dance).
- **`NormalizeLocale` BCP-47 segment-casing** (test-pinned `zh_hans → zh-Hans`; forms catalog keys).
- **Runtime gamepad `DrainEngineInput`** — do NOT extract to a `GamepadRouter`; only the diagnostics
  half of the Runtime split is safe.
- **EngineInput routing reset (`:296-306`)** — behavioral, not diagnostic; keep as `ResetSessionRouting()`
  on the close edge.
- **`MenuEventSink::s_consoleOpen`/`ConsoleOpen()`** + LoadingMenu/MainMenu force-hide — behavioral.
- ~~**`D3D12Compositor::ModuleFileNameLower`'s basename extraction**~~ — **RETIRED 2026-07-29: the
  code this protected no longer exists.** `c28ce3b` removed module-name Frame-Generation detection
  outright along with the present-hook path; `grep -rnE 'ModuleFileNameLower|dlss_g|dlssFg|FrameGenActive'
  src tools` returns **zero hits** at HEAD. That same commit added §2.6 above, so the plan was
  contradicting itself. FG target selection now lives in `src/composite/D3D12Compositor.cpp:609-638`
  (ordinary UI target with FG off, transparent COPY_SOURCE hand-off with FG on) and
  `docs/seam-draw-design.md` carries the current statement — "Frame Generation no longer suspends
  drawing". Do **not** reintroduce module-name sniffing to satisfy this bullet.
- **`Runtime.cpp` args handling (~2250-2267)** — do NOT apply `GetStringArray` (coerces non-string args).
- ~~**`ThreadAffinityProbe`/`NoteRuntimeTick`** — do NOT delete now; validates the just-landed `7ab68ad`
  fix; follow keep-then-remove cadence.~~ **RESOLVED 2026-07-29: cadence complete, probe deleted**
  (findings recorded, fix confirmed, backtrace proven mislabeled). See audit §7.
- **Wire-facing strings are a frozen contract, byte-identical** — bridge names + seam/present literals
  (`seamMode`, `'ui-seam'`, `'present'`). Centralize the *symbol*, never the *value*.
- **`FocusMenu` `kFlag` constants** — if trimmed, relocate full RE provenance to the `.cpp`.
- **`padnav.js`** — frozen compatibility boundary; only touch in a sanctioned in-game-verified pass.
- **Pipe message-builder helpers** must take primitive value args, not `ViewRec&` (data-race).
- **`PatchVtableSlot`** must not source the chain pointer from its return value (store-before-write).
- **HostApp out-of-process boundary** + `RunHost`/`Run` pump; winrt STA init (`:2778`) + STA affinity of
  every WebView2/composition call; DirectComposition visual tree; WGC capture session.
- **HostApp shared-texture ring** with produce/consume `ID3D11Fence` sync + `ringMutex` (`:290-317`) —
  capture-thread produces, STA tears down; the mutex must survive any `FrameTransport` extraction.
- **HostApp layered egress/security** — `WebResourceRequested` 403 filter + `kNeuter` removal + focus
  CDP; JS stays compiled INTO the exe; the four `AddScriptToExecuteOnDocumentCreated` calls keep order
  (neuter first).
- **HostApp `s_app`/`s_hostInputApp` singletons** — WNDPROCs can't carry instance state; keep the
  singleton pointer.
- **`IUiModule`** — KEEP (not remove, not a DLL seam): ordered lifecycle fan-out; diagnostics
  deliberately last; `_diagnostics` constructed before `SettingsModule::OnStart`.
- **WebView focus watchdog** (`:1145-1198`) — KEEP (Chromium MoveFocus race, independent of tick thread).
- **MessageBridge entry-time in-flight reset** — keep (only the *trailing* reset is redundant);
  MessageBridge stays transport-only.

---

## 5. Corrections applied to `SIMPLIFICATION_AUDIT.md`

1. **do-not-touch #10 (pause-menu):** split the bundle — keep the liveness/count gates (load-bearing,
   predate the debounce), downgrade the `kListStableTicks` debounce alone to *validate-then-remove*
   (see §2.1). Corrected in that file.
2. **Scope caveat** added to the audit: it covered only `src/` + `frontend/`, not
   `tools/webview2_host/`, `docs/`, or protocol names — so those items here are **additive**, not
   conflicts.
3. **Pointer to this plan** added at the top of the audit.

Remaining net-new items introduced here (not in the audit's scope): the six `HostApp.cpp` extractions,
the IUiModule comment fix, `architecture.md`/dangling-docs hygiene, protocol-name centralization,
`ResetSessionRouting` precision on §6.5, and the watchdog distinction.

---

## 5b. Execution log

Work landed on branch `simplify/phase-1` (branched from `main` at `e4db141`). Every commit was
validated independently against the gates that apply to the files it touches: `xmake build` (full
plugin), `bash tests/native/run.sh`, and `npm --prefix frontend run verify`.

### Phase 1 — complete
| Commit | Item(s) |
|---|---|
| `d0fff12` | §6.1–6.5 dead code + diagnostics-only removals; `LogSessionSummary` → `ResetSessionRouting` (behavioural reset preserved, §2.4) |
| `f3ba3b3` | IUiModule comment honesty, orphaned doc comment, **pause-menu CTD attribution corrected** (§2.1), `architecture.md` draw-path + IUiModule |
| `58ab7c0` | §5.9/5.10/5.11 frontend dedupe; §6.7 dead `cycleRail` |

**Not done — needs a decision (§3 Phase 1, "dangling design docs").** Git shows `mcm-design.md`,
`api-freeze-plan.md`, `reverse-engineering-notes.md` and `docs/ROADMAP.md` were **deliberately
deleted** in cleanup commits (`c20163f`, `901107f`, `0126172`); `form-references-design.md` and a
root `ROADMAP.md` were never tracked. So "restore from git history" would revert intentional
deletions. The alternative is rewriting ~38 citations across ~30 files, which loses the rationale
each `§`-pointer referenced. Left for the maintainer to choose.

### Phase 3 — §4b shared helpers: complete
| Commit | Item |
|---|---|
| `d22943b` | §4b.1 `core/StringUtil.h` (6 sites; `ModuleFileNameLower` basename kept per §7) |
| `46fca00` | §4b.3 `Json::CheckFormatVersion` (4) + §4b.4 `GetStringArray` (4; Runtime arg-coercion excluded per §7) |
| `04e1e47` | §4b.5 `Ids::ModOf`/`ViewNameOf` (5 sites) |
| `2d19a3b` | §4b.6 `KeyName` derived from `kNamedKeys` |
| `d83a9bc` | §4b.2 `Version::IsTargetNewerThanHost` (scoped to the pure predicate — see below) |

### Phase 3 — §4c native copy-paste: partial
| Commit | Item |
|---|---|
| `8040be0` | §4c.4 `SettingsMirror::LookupMod`/`LookupKey` (guardrails kept: null-check before `string_view`; empty-key branch caller-side) |
| (follow-up) | §4c.4's acceptance tests. `8040be0` shipped with the refactored paths at **zero** coverage — `ResolveNames` had no test at all and nothing fed the mirror a case-variant id or key, so its "verified by settings_mirror + settings_subscriptions tests" claim did not reach the changed code. `settings_mirror_tests.cpp` now covers empty-key whole-mod-resolve and exact-beats-case-variant through both entry points (`ResolveNames` and the typed getters' `Find`). Mutation-checked — each of these was applied, confirmed to turn the suite red, and reverted: CI-before-exact in `LookupMod`; CI-before-exact in `LookupKey`; removing `LookupMod`'s CI fallback; removing the `a_outKey.clear()` on the empty-key path; writing `a_outMod` on a failed lookup. Deliberately no failure COUNTS recorded: `_mods` is an `unordered_map`, so how many assertions a given mutation trips depends on hash order and is not a stable property — "goes red" is, and the assertions that could legitimately resolve to either case-variant accept both. |
| `11bc8a8` | §4c.3 `SettingsStore::CollectConflicts` (`selfBlocksGameplay` as a parameter) |
| `6ad515d` | §4c.7 `PapyrusApi::DispatchOne` |
| `501ec5b` | §4c.1 `ReloadViewInPlace` (+ `BroadcastViewsData` moved after the reload core) and §4c.2 `IsViewReady`. ⚠ The commit message justifies the move as avoiding a "stale 'loaded' state" — that premise is **false**: the pre-extraction code already set `Loading` before broadcasting, so the payload is identical either way. The real (benign) delta is wire ORDER — the `views.data` push now follows the navigate/resize instead of preceding them, and the only view whose position changes is the one being navigated away. Comment corrected in the tree. |
| `2cbcfad` | §4c.11 `composite/D3D12Prologue.h` |
| `2c19887` | §4c.8 action registrars → 2 shared bodies + `ValidateActionModId` |

**Evaluated and declined** (recorded so nobody re-opens them without new information):

- **§4c.10 `EdgeLatch`** — declined. `FreeCursor::Apply` and `SimPause::Apply` share ~8 lines, but a
  shared latch needs a state bool plus an `acquire` callable and an `apply` callable (different
  singleton types, different effects, different logs) — as long as the duplication it removes, across
  exactly two ~30-line self-contained functions. The plan already sanctioned this outcome.
- **§4c.9 subscription-registry base** — declined. The genuinely identical parts are `Unsubscribe`,
  the token-mint loop, and the per-call liveness re-check (~15 lines). Sharing them needs a template
  base, which forces both classes' nested `Subscription`/`Event` types out to namespace scope: two
  self-contained files become three for roughly a wash in line count. A shared base for **two**
  implementations is premature. (The one real readability win, `IsLive(token)` instead of an inline
  scoped-lock block, is not worth the restructuring on its own.)
- **§4b.2 partial scope** — the `AcceptTargetVersion` logging helper was not added. The
  ViewManifest/SettingsStore parse-and-store blocks need `ParseDottedVersion` for their
  format-validity branch regardless, and a logging helper would pull REX logging into the otherwise
  dependency-free `Version.h`. Only the pure predicate was shared.

### §4c.5 — pipe message-builders (landed, `3e5b049`)
Eight wire shapes were authored twice (snapshot vs setter) and parsed by a *separate binary*, so
drift was silent. One builder per shape; each shape now has exactly one authoring.

The race guardrail is **structurally enforced** rather than comment-enforced: the builders live in
the anonymous namespace, which closes *before* `Impl::ViewRec` is declared, so a `const ViewRec&`
signature cannot compile there. Two checks worth repeating on any future edit:
- a per-shape grep must return exactly one authoring each;
- `accelState`'s adjacent bools — the locals are `accCaptured`/`accArmed` but the wire keys are
  `captured`/`captureArmed`, so a swap compiles clean and silently changes the wire.

⚠ **`xmake build` alone does not prove this file compiled** — it is behind
`#if defined(OSFUI_WITH_WEBVIEW2)` (option defaults on, `xmake.lua`). Confirm the compile output
actually names `WebView2HostWebRenderer.cpp`. `tests/native/run.sh` never compiles it.

### §4c.6 — `PatchVtableSlot` — CANCELLED for the compositor
The compositor no longer patches `IDXGISwapChain::Present`, so the proposed shared helper has no
compositor caller or in-game benefit. `UiPassSeam` keeps its own fail-closed slot patching and
original-pointer publication rules; do not generalize them as part of the retired Present work.

### Phase 3 — §5 frontend consolidation: all but the two race-sensitive items
| Commit | Item(s) |
|---|---|
| `aa6a072` | §5.8 single `HOME_ID` owner (new leaf `lib/ids.ts`); §5.12 `workloadOf`; §5.13 `inputP95` reuse; §6.9 `execCommand` fallback dropped, try/catch kept |
| (follow-up) | §5.15 `optAttr` — the other half of §5.15, landed late (see below) |
| `dae3b29` | §5.3 `seedBaseline` + §5.4 `patchModValues` (3 copies each) |
| `7082991` | §5.5 `useCommittedText` |
| `eef7565` | §5.6 `HudCard`→`Mark`; §5.7 `BrandEmblem`; §5.14 `ViewRowText` |
| `505f036` | §6.8 `data-label`/`data-mod` removal |
| `c52258b` | §5.15 `cx()` |
| (with the above) | §5.13 `TimedSummary` shared by both result shapes |

Notes worth keeping:
- **§6.8 was verified before deleting**, not taken on faith: no CSS selector, no `padnav.js`, no shipped
  kit, no docs/sdk reference — the only reader was a self-fulfilling test assertion. The code agreed
  (`labelText`'s doc said "for the (removed) DOM filter"; Rail's attribute carried "Nothing reads this
  any more"). `data-key` **stays** — it is the live search-jump anchor.
- **§5.15 `cx()` argument order is a convention the DOM-contract tests pin** — and be precise about
  why, because the original wording here was wrong twice over. It claimed padnav "selects on the
  trailing state classes (`.listening`, `.pending`)". padnav does neither. It reads classes in
  exactly two places, both order-blind: `el.closest('.row')` for banding (`padnav.js:79` — the
  load-bearing one, and the contract `Row.tsx` documents) and `document.querySelector('.listening')`
  as a presence test (`padnav.js:184`). It never queries `.pending` at all — and class order inside a
  `class` attribute is invisible to both CSS matching and `querySelector` regardless.
  It then claimed the DOM-contract test guarded the ordering; that test mounts the **keybinds** view,
  whose `HolderRow` still hand-writes the string, so it never rendered the migrated `@ui/KeyField`
  or `@ui/ActionButton` at all — either could have been reordered with all tests green.
  `dom-contracts.test.tsx` now renders both kit components directly and compares `className`
  verbatim (mutation-checked: reordering `KeyField`'s `cx()` args fails the suite). The ordering is
  worth keeping for consistency and to make those assertions writable — it is not a padnav contract.
- **§5.15 `optAttr` landed late.** `c52258b` shipped only the `cx()` half; `optAttr` was neither
  implemented nor recorded as declined, which is what made the "Phase 3 complete" claim below wrong.
  It now exists at `frontend/src/ui/optAttr.ts` and carries the consolidated
  `exactOptionalPropertyTypes` rationale §5.15 asked for. Applied to the two conditional-attribute
  sites inside §5.15's named scope (`Badge` `title`, `Row` `data-key`). Six further sites use the
  same idiom outside that scope and are deliberately left alone for now: `Switch` (`id`),
  `Detail` (`id`, `title`), `Presets` (`title`) would migrate cleanly; `Overlay` (`onClick`),
  `ImageRow` and `marks` (`style`) should NOT — a handler or style object spread through
  `optAttr`'s index signature loses its precise type.
- **§5.3's `ensureEntry` flag** is load-bearing: only the whole-list capture may seed an entry for a
  mod with no values. Folding that into the shared path would make a zero-key preset force a render.
- **§6.9 is a deliberate BEHAVIOUR CHANGE, not a cleanup** — the only *user-visible* one on this
  branch, so it must not be filed as tidying. (`501ec5b`'s views.data wire-order shift is also
  observable, but only to a view being navigated away — see the §4c.1 row.) Dropping the
  `execCommand` fallback means a `navigator.clipboard.writeText` rejection no longer copies anything.
  Scope it honestly: an earlier draft of this note claimed the overlay "normally" lacks focus in
  game, and that is **wrong** — the benchmark manifest is `kind: "menu"` with `capturesInput: true`,
  so `Runtime::ReconcileNativeFocus` grants it native focus for its whole visible session. A
  rejection is reachable (clipboard permission policy, or the asynchronous focus grant losing the
  race the host's focus watchdog heals) but it is not the common path, which makes this a smaller
  regression than first reported. The commit message's premise — that the old path produced "a false
  'Copied'" — is plausible (`execCommand` wants document focus too) but was never demonstrated.
  Resolution: keep the removal (the deprecated API stays gone), but the failure path now surfaces the
  JSON in a selectable, auto-selected `textarea` that persists until dismissed, so the data is never
  taken away from someone who has to copy it by hand — the same principle as `Health.tsx`'s
  `copyText`. It is cleared by Clear and Run-suite, which discard the results it describes.

**§5.1 `useStateRef` / `useLatest` — landed (`e4e685a`).** Eight state+ref+dual-write triples and
three render-synced mirrors across the two views are now declarative. The ordering rule is the
load-bearing part and is documented on the hook: **the setter writes the ref BEFORE the state**, so a
closure running later in the same tick sees the new value and back-to-back bridge pushes compose.
`applySave` builds on the hook rather than collapsing into it — it calls `setSave` for the pair and
keeps its own fade-timer bookkeeping, including the deferred `saveRef.current` read inside the
timeout.

`e4e685a` left **two** render-synced mirrors in `settings/App.tsx` hand-written (`toastRef`,
`undoOpenRef`) while converting the byte-identical pattern in `keybinds/App.tsx` — an inconsistency,
not a behaviour difference. Both are now `useLatest`. The remaining `useRef`s in these three views
are genuinely out of §5.1's scope: DOM handles (`filterInput`, `searchRef`, `stageRef`, `canvasRef`,
and `manualCopyRef`, added by the §6.9 follow-up above), a timer handle (`fadeTimer`), and plain
mutable state with no `useState` pair (`padRef`, `pendingHashSelect`, `hashRead`, `activeRef`,
`suiteTokenRef`). None of these mirrors a `useState` value, which is what §5.1 was about.

One latent hazard the hook introduces, harmless today: `useStateRef`'s setter is re-created every
render rather than `useCallback`-stable like a raw `useState` setter. No dependency array in any of
the three views lists one, so nothing re-runs — but adding a setter to a `useEffect`/`useMemo` dep
list would now loop.

**Still open in §5 — §5.2 only:**
- **Reuse `useCapture` in keybinds** (~-90 lines, the highest-value §5 item): a duplicated
  race-sensitive capture state machine. It needs an **in-game controller smoke** that cannot be run
  headlessly, so it belongs with the other in-game-gated work. When executed:
  generalize `CaptureTarget` to an opaque instance token (keybinds keys by `instanceId`, settings by
  `mod:key`) and expose `capturingId`; make the conflict-toast i18n address a hook **option** so
  keybinds keeps `alsoBoundBy` while settings keeps `capturedAlsoBound` (naive reuse silently orphans
  a catalog entry); route **all** post-commit work through `onCommit` — `setMods`, `settings.set`, the
  `rebindRejected`/get fallback, `setSelectedKey`, `setLoaded` — not just `setMods`. Verify the
  keybinds catalog still resolves `alsoBoundBy`.

### Protocol-name centralization — DECLINED after survey (evidence below)
The plan called for one C++ `Protocol.h` + a mirrored TS module replacing the scattered wire-name
literals. Surveying the actual surface first changed the answer:

1. **No drift exists today.** Every native→web type C++ emits is declared in the SDK's
   `NativeToWebMessage` union. Nothing is emitted-but-undeclared.
2. **The "23x / 16x" duplication counts came from tests**, not production. Production uses each name
   only **1–2 times** (C++ `src` ~40 literals, `frontend/src` 40; `frontend/test` 229 and
   `tests/native` 121). Literals in tests are *correct*: a test that shares a constant with the
   implementation cannot catch a wrong value.
3. **These names are frozen wire contract** — consumed by the byte-compared `osfui.js` and by
   third-party mods. They will never be renamed, so the rename-drift risk that normally motivates
   centralizing constants does not apply here.
4. **It would work against readability.** At a handler, `RegisterCommand("settings.get", …)` is more
   intuitive than `RegisterCommand(Protocol::kSettingsGet, …)`: the wire name *is* the domain
   vocabulary, and the indirection forces a jump to learn what actually goes on the wire.
5. **The failure mode is bad.** A value typo while swapping ~80 sites silently breaks the native↔web
   contract with no compile-time signal — the worst outcome for a frozen public protocol.
6. **The one guard worth having cannot be made reliable.** A cross-layer test (assert every emitted
   type is declared) has to grep C++ source, and emission paths vary — `SendToWeb` overloads,
   multi-line calls, and `PushToSubscribers`. A careful regex written for this survey produced **four
   false negatives** (`i18n.data`, `settings.changed`, `settings.persisted`, `ui.hotkey` all *are*
   emitted). A source-grepping test that unreliable is not worth having — but see the retraction
   below for why that does not extend to every possible guard.
   (This list originally read "five", counting `ui.command`. That was the same error as the retracted
   finding below: `ui.command` is never emitted native→web. Its only appearance in `src/` as a
   message TYPE is `MessageBridge.cpp:65`, matching an INCOMING web→native message; the other six
   hits are a log string and doc comments.)

**Retracted — the "net-new finding" was wrong.** An earlier revision of this section claimed
`ui.command` appears in the SDK's `NativeToWebMessage` union and should be removed. It does not.
That union is 18 members (`sdk/osfui.d.ts:490-508`) and `ui.command` is not among them; its only
declaration is `WebToNativeMessage` (`sdk/osfui.d.ts:122`) — already the correct direction. Acting on
the original note would have deleted a correct declaration from a public `.d.ts`. Nothing to do.

Worth drawing the obvious lesson: the survey that declined centralization on the grounds that "no
drift exists today" itself shipped a factual error about that very union. That does not overturn
points 1–5 above — the wire names really are frozen, really are 1–2 uses each in production, and a
value typo really would be silent. But it does soften point 6: the reason a cross-layer guard was
rejected is that a *source-grepping* test is unreliable, and that is true. A hand-maintained C++ name
list asserted equal to the SDK union is weaker than what the plan originally wanted, but it is not
"worse than none" — it is exactly the check that would have caught this.

### Still open
**Phase 3 is complete** — as of the follow-up pass, not as of `204dcde`. Everything in it either
landed, or was declined/deferred with the reasons recorded above (§4c.6 deferred; §4c.9, §4c.10 and
protocol-name centralization declined).

When this section first claimed completeness it was not true, and the gaps were invisible because
nothing recorded them either way. A post-branch review found three, all now closed: §5.15's `optAttr`
half was never implemented and never declined; §5.1 left two of its own sites unconverted; and
§4c.4's stated acceptance tests were never written, so its "verified by" claim did not reach the
refactored code. **Rule for the next pass: an item is closed when it has landed OR has a written
decline — a table row naming the commit is not by itself evidence that the item's own acceptance
criteria were met.** Where a §5b entry claims a test guards something, name the test and check it
actually renders the code in question; two of the claims here did not.

Every remaining item needs the game running, so they form one batch for a session at the keyboard:
- **Phase 2** — the two thread-defense retirement experiments (pause-menu debounce; instrument the
  admitted-state watchdog), each with the FSR3-FG-ON acceptance run.
- **§5.2** — reuse `useCapture` in keybinds (controller smoke).
- **§4c.6** and **§6b.2** — seam / Present-slot changes sharing the same in-game smoke.
- **Phase 4** and **Phase 5** — the god-object decompositions (`RuntimeDiagnostics` first, then the
  `HostApp.cpp` sequence), which have no headless harness and need an overlay smoke per extraction.

---

## 6. Provenance

Produced by two orchestrated workflows (75-agent subsystem audit → 7-agent verification+fusion) over
commit `e4db141`. All actionable HostApp/IUiModule/IWebRenderer/protocol locations are cited from the
local HEAD tree, not from Codex's macOS report. Line references are approximate — re-anchor by symbol
if HEAD advances.
