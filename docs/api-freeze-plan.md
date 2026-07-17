# Pre-1.0 API Freeze Plan

Working plan for the pre-first-release breaking-change audit (2026-07-17). The
audit swept the three public surfaces — the `window.osfui` bridge, the native
plugin API (`sdk/OSFUI_API.h`), and the data/persistence formats — for designs
that become unfixable once third-party mods ship against them. Each item below
gets designed (decisions recorded here), then implemented.

Status key: ✅ designed · 🔨 implemented · ❌ not started

---

## 1. ID namespacing convention — 🔨 implemented (2026-07-17)

**Problem.** Mod ids and view ids live in one flat global namespace with silent
first-wins collisions (`SettingsStore::LoadAll` sorted-filename first-wins; view
id == folder name under `views/`, so two mods shipping `views/hud/` collide at
the mod-manager VFS layer before the runtime even sees them). Id matching is
case-sensitive on a case-insensitive filesystem. Unfixable after third-party
ids exist.

### Decisions

| Decision | Choice |
|---|---|
| Mod id format | `author.modname`, pattern `^[a-z0-9-]+\.[a-z0-9-]+$`, author = self-allocated handle (Nexus/GitHub) |
| View layout | Nested `views/<modId>/<viewName>/manifest.json`, view name pattern `^[a-z0-9-]+$` |
| Qualified view reference | `mod/view` with a **slash** (`ozooma10.almanac/planets`) — slash because mod ids contain a dot, so a dotted join is ambiguous to split; the slash mirrors the folder path |
| Enforcement | **Hard-reject at load**: schema/view failing the pattern is skipped with an ERROR naming the file and the rule. Same validation at the native API boundary (`RegisterView`, schema registration) |
| Collisions | Deterministic first-wins by sorted name; loser skipped with an ERROR naming both paths; conflict carried as an additive field in `settings.data`/`views.data` so the Mods surface can render a conflict badge |
| Casing | Pattern enforces lowercase at load, so existing case-sensitive compares become correct for free |

### Properties that fall out

- **Dotless = platform-reserved by construction.** No valid third-party mod id
  can lack a dot, so `osfui`, `shared`, and the existing reserved ids need no
  grandfathering logic beyond "built-ins may be dotless; third parties may not".
- Built-ins **dogfood the layout**: `views/osfui/settings/`,
  `views/osfui/keybinds/`, referenced as `osfui/settings` / `osfui/keybinds`.
- Shared kit stays top-level at `views/shared/` (dotless → reserved; the
  two-level scan skips it naturally — no `<view>/manifest.json` inside). Views
  reference `../../shared/osfui.css`; the post-URL-collapse form
  `file:///shared/osfui.css` is the frozen contract. The sandbox needs **no
  rework**: `SandboxFileSystem::Resolve` already serves one `_viewsBase` root
  depth-agnostically (`src/render/UltralightWebRenderer.cpp`).
- Manifest `mod` field becomes derivable from the path — keep it optional as a
  consistency check (mismatch = reject), not required.
- Pre-release: no user migration. Existing `osfui` settings values files keep
  their id/filename.

### Implementation checklist

Code:
- [x] `src/runtime/Ids.h` (new) — the one grammar implementation
      (`IsValidModId`, `IsBuiltInModId` = {`osfui`}, `IsValidViewName`,
      `IsValidQualifiedViewId`), shared by SettingsStore / ViewManager /
      ViewManifest / BridgeApi. Header-only so the host-side tests get it free.
- [x] `src/runtime/SettingsStore.cpp` — grammar validation in
      `ValidateSchemaShape` (ERROR; replaces both the old charset check AND the
      reserved-word list — every reserved name is dotless, hence invalid);
      grammar-violating filename stems hard-rejected in `LoadAll` with an ERROR
      naming the file; duplicate-id refusal upgraded WARN→ERROR naming both
      files; `Mod.shadowed` conflict record surfaced additively as
      `settings.data` `mods[].shadowed`. *Deviation:* the shadowed record is
      limited to file-vs-file collisions — native-over-drop-in is the
      documented tier-upgrade path, and badging it would flag every upgraded
      mod. (Note: since drop-in id == stem is enforced, file-vs-file duplicates
      can't currently occur in one merged dir — the record is the policy hook,
      live only via the hot-reload/native-owned refusal path.)
- [x] `src/runtime/ViewManager.cpp` — two-level scan; `shared/` skipped
      explicitly, manifest-less subdirs skipped silently (asset folders);
      old flat layout (manifest.json directly under views/<x>/) detected and
      ERROR'd with a migration hint; mod-folder grammar hard-reject.
- [x] `src/runtime/ViewManifest.cpp` — id/mod derived from the path; declared
      `id` ≠ folder name = reject; declared `mod` ≠ mod folder = reject;
      `manifest.id` becomes the qualified `mod/view`.
- [x] `src/runtime/Runtime.cpp` — no structural change needed: view ids flow
      as opaque strings through MenuController/bridge, so qualified ids work
      end-to-end once Config defaults (`osfui/settings`, `src/core/Config.h`)
      and `PauseMenuEntry`'s default target moved. RegisterView warn text now
      names the qualified shape. *Deviation:* no conflict field in
      `views.data` — view-id collisions resolve below the runtime (folder
      names are unique per directory; cross-mod collisions merge at the MO2
      VFS layer, invisible here). Only settings.data carries `shadowed`.
- [x] `src/render/UltralightWebRenderer.cpp` — `LoadView` URL is now the
      views-relative `file:///<mod>/<view>/<entry>` (last two rootDir
      components); `../../shared/osfui.css` collapses to the frozen
      `file:///shared/…` form, `SandboxFileSystem::Resolve` untouched.
- [x] `src/api/BridgeApi.cpp` — `RegisterView` refuses non-qualified ids
      synchronously (returns false + WARN), like the schema shape gate.

Data / shipped content:
- [x] `views/osfui/settings/` + `views/osfui/keybinds/` (git mv); `shared/`
      stays top-level. `xmake f` reconfigure run; deployed MO2 tree verified
      nested-only (no stale flat folders shipped).
- [x] `../../shared/osfui.css` links; settings view's mod-asset base moved
      `..` → `../..` (`safeAssetSrc`), harness override map keys unchanged.
- [x] `data/OSFUI/config.json` — qualified `view`/`views`
      (`pauseMenuEntryView` rides the Config.h default).

Tooling / docs / tests:
- [x] `devtools/harness/` — pages point at the nested paths; mockbridge
      catalog/`HARNESS_PAGES` use qualified ids, `validModId` mirrors the new
      grammar, fixture mods renamed to dotted ids (`acme.*`), action-ack
      derives the mod id from the payload (dotted ids broke first-dot split).
      *Interim:* OSF Animation entries use the target `osf.animation` names
      with a shim remapping its still-`osf` schema id until that repo's
      item-3 migration lands.
- [x] `tests/native/` — all fixture mod ids dotted (`t.alpha`, …); new cases:
      dotless/uppercase/underscore/two-dot rejection, `osfui` accepted,
      grammar-violating stem file-skip. All 7 suites green (162/162 store).
- [x] `docs/authoring-views.md` — new §0 (id grammar), nested layout, shared-
      kit link rule, qualified references throughout.
- [x] `docs/native-plugin-api.md` — `RegisterView` qualified-id contract +
      synchronous refusal; worked example uses `osf.animation/browser`.
- [x] `docs/schema/manifest.schema.json` — id/mod patterns (id = view folder
      name; qualified id documented as derived).
- [x] `docs/schema/settings-schema.schema.json` — id pattern + filename rule.
- [x] `examples/settings-only/mymod.json` → `yourname.mymod.json`
      (+ README, command namespace `yourname.mymod.*`).
- [x] `README.md` + `sdk/README.md` + `sdk/osfui.d.ts` path/id references.

Still open (tracked elsewhere):
- ~~OSF Animation repo migration~~ — landed with the item-3/4 slice (their
  0d84f92; see item 3's checklist).
- ~~In-game verify pass~~ — passed 2026-07-17 (fresh boot: nested views load,
  F10 opens `osfui/settings`, pause-menu entry opens it, keybinds terminal
  opens, shared CSS resolves; OSF Animation card + browser under the new ids,
  `osf.animation.*` command + hotkey round-trips work).

Follow-on alignments (tracked as their own items below):
- Bridge **commands** registered by native plugins should be required to start
  with `<modId>.` — also solves the reserved-prefix shadowing hole (item 3).
- The hardcoded theme-class enum in `views/shared/osfui.css` should become
  "accent keyed by mod id" (item 9).

---

## 2. Settings-type forward compatibility — 🔨 implemented (2026-07-17)

**Problem.** `SettingsStore::Validate` is a closed type set; worse, the
load-time prune (`saved != SparseValues(mod)` → rewrite,
`SettingsStore.cpp` ~316-324) **permanently deletes** any saved value whose
setting has an unknown `type` or whose key left the schema. Run a new-schema
mod on an old OSF UI once → user settings wiped from disk. The web view
already degrades correctly (read-only `unknownRow`: "needs a newer OSF UI") —
the native store is the whole problem.

### Decisions

| Decision | Choice |
|---|---|
| Preservation | **Preserve what the host can't understand**: unknown-typed settings and unknown keys round-trip opaquely in the values file (kept verbatim on every rewrite, never served). Known-typed values keep today's behavior (clamp, enum-removal → resolve/prune as now) |
| Served value | **Schema default; preserved value withheld.** Consumers (views via `settings.data`, native plugins via `SubscribeSettings` replay) only ever receive store-validated values — the security-model invariant stands. The mod's plugin gets its declared default until the user upgrades |
| Type set | **Add `flags` now, then freeze.** Implement multi-select `flags` (value = array of option strings; validate ⊆ options, deduped; checkbox-group widget) natively before release. Kill the promised first-class `color` type — `type:"string"` + `widget:"color"` (already shipped, `SettingsStore.cpp` ~690) is the pattern. Frozen base set: `{bool,int,float,enum,string,key,flags}`. Post-1.0 extension = base type + `widget` + attributes; a genuinely new base type requires the schema gate below |
| Schema gate | **Capability list, reserved now**: schema-level `"requires": ["type:flags", ...]` (named capabilities, matching the `runtime.ready` capabilities direction, item 6). Unmet → the mod registers as a stub card ("Needs a newer OSF UI"), values file untouched, nothing served. Hosts older than the field ignore it — shipping it in the first release is the point |

### Implementation checklist

- [x] `src/runtime/SettingsStore.cpp` — `Mod.preserved` opaque bag: filled at
      load from (a) declared settings with an unknown type (saved value kept,
      default served, `Set` refused) and (b) saved keys no schema fact
      accounts for; merged into `SparseValues` so every rewrite carries the
      opaques verbatim, and the load-clean check includes them (a file whose
      only oddity is unknown content opens NO rewrite window). Known-typed
      keys keep today's behavior exactly (clamp/alias/prune). *Detail:* an
      unknown-typed setting's `aliases` are deliberately NOT accounted — this
      host can't adopt them, and dropping them would strand a rename for the
      newer host that can. `flags` case in `Validate` (array over `options`;
      resolve, don't reject: filter unknowns + dedupe + canonicalize to
      declared-option order — the enum-removal analogue of clamping).
      `requires` evaluated at registration → stub Mod (inert: values empty,
      `Set`/`Reset`/`GetSettingType` refuse, never dirtied, values file never
      read or written); `settings.data` entry gains additive `stub: true` +
      `missingRequires`. New `src/runtime/Capabilities.h` holds the
      append-only named-capability set — the same list item 6 will emit in
      `runtime.ready`.
- [x] Settings view — `buildFlags` checkbox group (`.osf-flags` kit styles in
      shared css; commits the full array in canonical order); stub card in
      the detail pane ("needs a newer OSF UI" + missing list; Reset-all
      hidden; the mod's terminals still listed — they register independently
      of the settings gate). Structural (JSON) equality shims for array
      values in `isModified`/session-undo/settings.changed echo detection.
- [x] `devtools/harness/mockbridge.js` — `flags` validate case, `CAPABILITIES`
      mirror of Capabilities.h, `requires` → stub in `buildMod`, stubs inert
      for set/reset.
- [x] Contract lockstep: settings-schema.schema.json (`flags` type +
      top-level `requires`; the future-`color`-type promise replaced with
      "colour is string+widget, base set frozen"), `sdk/osfui.d.ts`
      (`SettingType`, `SettingValue` gains `string[]`, `SettingsSchema.requires`,
      `settings.data` `stub`/`missingRequires`/`shadowed`),
      authoring-views.md (type-rules row + forward-compat/`requires` section),
      native-plugin-api.md §5a (replay/getters = default when the host
      predates a type; stub = nothing served; flags via JSON callback, no
      typed getter). `examples/settings-only/yourname.mymod.json` gained a
      `flags` demo row.
- [x] Native tests (settings_store_tests, all green 192/192): flags
      validation (resolve/dedupe/canonical order/reject non-array),
      preservation round-trip (unknown type + unknown-type alias + unknown
      key: served defaults, clean load byte-identical, opaques survive a real
      rewrite, never in `Data()`), requires-gate stub (met vs unmet, inert
      surface, `stub`/`missingRequires` payload, values file byte-untouched);
      legacy prune test updated — unknown keys now preserved, not wiped.

---

## 3. Command namespace + registration policy — 🔨 implemented (2026-07-17, incl. the OSF Animation migration)

**Problem.** `IsReservedCommand` (`src/api/BridgeApi.cpp`) blocks only
`ui./runtime./game./settings./views.` — `menu.*`, `hud.*`, the bare platform
verbs (`close`, `setVisible`, `setViewHidden`, `log`, `ping`,
`osfui.gamepadRaw`) are shadowable, registration is last-writer-wins, and the
shipped header documents the list without `views.` (already drifted).

### Decisions

| Decision | Choice |
|---|---|
| Command shape | **Require `<modId>.<name>`** — RegisterCommand refuses anything not shaped `author.modname.command` (two dots minimum). Since item 1 makes every mod id contain exactly one dot, ALL platform commands (dotless verbs, single-dot `menu.open`/`game.get`) are structurally unregisterable — the reserved-prefix list is deleted, not extended, and can never drift again. Mod id must be pattern-valid but need not have a registered schema |
| Duplicates | **First-wins, refuse duplicates** with a WARN naming the command. Handler replacement = explicit `UnregisterCommand` then re-register (both already in the ABI). Consistent with item-1 collision policy; hijack impossible instead of logged |
| Platform verbs | Stay as-is (`close`, `menu.open`, …) — no `ui.` migration churn; the two-dot rule protects them structurally |
| OSF Animation | **Renames to `osf.animation`** — the OSF family claims `osf` as its AUTHOR segment (`osf.animation`, later `osf.seduce`, `osf.almanac`, `osf.body`); documented as a claimed prefix. Migration in the OSF Animation repo: schema id, `RegisterView` id, `views/osf/` folder, commands → `osf.animation.*`, values file `osf.json` → `osf.animation.json` (hand-migrate or reset own saved values — the alias mechanism covers key renames, not mod renames). Zero public users; only cheap moment |

### Implementation checklist

- [x] `src/api/BridgeApi.cpp` — `IsReservedCommand` DELETED, replaced by
      `IsValidPluginCommand` (`<modId>.<name>`: split at the SECOND dot, the
      leading `<author>.<modname>` must pass the item-1 grammar, the name may
      itself contain dots — `acme.mymod.catalog.get`). Duplicate → refuse +
      WARN naming the command (first-wins); Unregister→re-register within one
      tick still replaces (the pendingUnregister bookkeeping already handled
      it). ABI bumped to **1.6** with the guarantee documented in the header's
      History like 1.3's.
- [x] `sdk/OSFUI_API.h` + `docs/native-plugin-api.md` — reserved-prefix prose
      replaced by the command-shape rule everywhere (interface comment, §7
      guards, §11 test rows; §12 "namespacing" open question marked resolved).
- [x] OSF Animation repo — the `osf` → `osf.animation` migration landed
      (their 0d84f92, path-limited): schema id + values-file orphaning noted,
      `views/osf.animation/browser/` (manifest id `browser`), RegisterView
      qualified id, ALL commands + message types `osf.animation.*`, vendored
      header 1.6 consumed via the `Client` wrapper, xmake deploy retargeted
      (+ removes the stale `views/osf/` from the MO2 target, which the item-1
      loader would otherwise ERROR on every boot). Schema `icon` became
      `browser/osf-icon.svg` — mod assets now resolve under the NAMESPACE
      folder (`views/<modId>/<path>`), not the view folder.
- [x] `docs/authoring-views.md` — claimed author prefixes documented in §0
      (`osfui` platform-reserved dotless; `osf` = the OSF family's author
      segment); the JS-side "no escape hatch" callout now states the
      two-dot plugin-command shape.
- [x] Tests: new `tests/native/bridge_api_tests.cpp` (39 checks — compiles
      the REAL BridgeApi against the host stubs; wired into run.sh + the
      MSVC batch): shape refusals incl. every platform verb + `osfui.*`,
      dotted-name acceptance, first-wins refusal, dispatch round-trip
      through a real MessageBridge, unregister-then-reregister replacement,
      qualified RegisterView ids, Client wrapper gating.

---

## 4. Native API version-gate + ABI hardening — 🔨 implemented (2026-07-17)

**Problem.** `OSFUI_RequestBridge` checks MAJOR only and returns the full
singleton (`src/api/Exports.cpp`); a client built against a newer header that
calls a tail vmethod on an older host without checking `GetInterfaceVersion()`
calls past the end of the vtable — UB/crash, and the gate is spatially far
from the call so it *will* be forgotten. Plus assorted header-contract gaps
found in the audit.

### Decisions

| Decision | Choice |
|---|---|
| Gate mechanism | **Header-side inline wrapper class as the primary documented API** (`OSFUI::Client` or similar): fetches the bridge, caches `GetInterfaceVersion` once, exposes the same methods; too-new calls return false/no-op instead of jumping off the vtable; `Has(Feature)` query for capability checks. Raw `IOSFUIBridge` stays available for advanced use; docs + examples use the wrapper exclusively; OSF Animation migrates to it (riding the item-3 `osf.animation` migration) |
| Multi-MAJOR policy | Documented now in the header: the factory is a per-major dispatcher — a future 2.0 host keeps vending the v1 interface to v1 callers. No code until a 2.0 exists; the policy statement is what prevents painting into the corner |
| ABI ground rules (header comments, pre-committed) | Any future struct crossing the boundary leads with `uint32_t size` set by the caller; enums/flags crossing the boundary are append-only, never reordered, 0 = unknown; vmethods append-only at the vtable end |
| Portability guards | `static_assert(sizeof(void*) == 8)` + "x64, MSVC ABI (MSVC/clang-cl), C++17 minimum" stated in the header |
| Standalone header | Guard the `REX/W32/KERNEL32.h` include so a plain-Win32 consumer can build without CommonLibSF (fallback to `<libloaderapi.h>` or user-supplied loader fns) |
| Lifetime contracts into the header | Callback `const char*` params valid only for the duration of the call (copy to retain); handlers may fire for process lifetime after registration (never point them at objects you free); settings replay may deliver duplicates — callbacks must be idempotent |
| Doc drift | Fix `sdk/README.md` protocol "0.1" → single-source; header's `GetBridgeProtocolVersion` example string; note in the header that the C ABI is the stable contract even while plugin/protocol versions are pre-1.0 |

### Implementation checklist

- [x] `sdk/OSFUI_API.h` — `OSFUI::API::Client` wrapper (caches the host MINOR
      once at Init/Attach; every tail method gates to false/0/no-op;
      `Has(Feature)` with Feature values = the introducing MINOR; `Raw()` for
      advanced use; `Attach(IOSFUIBridge*)` for test doubles). ABI ground
      rules + multi-major policy + lifetime contracts as header prose;
      `static_assert(sizeof(void*) == 8)`; REX include guarded — a plain
      Win32 consumer falls back to lean-and-mean `<Windows.h>`
      (`OSFUI_API_NO_REX` forces it), and the loader/RequestBridge section is
      `_WIN32`-only so host-side unit tests compile the header on Linux CI.
      Protocol example strings fixed ("0.4", marked informational — gate on
      the ABI MINOR, never parse the protocol string).
- [x] `docs/native-plugin-api.md` — wrapper-first §10 example (static Client,
      `osf.animation.*` command names); multi-major policy + ABI ground rules
      in §4; Appendix B command names updated.
- [x] `src/api/Exports.cpp` — logs every vend with the caller's requested
      ABI MAJOR.MINOR vs the host's (and WARNs a refused MAJOR mismatch).
- [x] OSF Animation — vendored 1.6, both consumers (`UIBridge.cpp`,
      `UISettings.cpp`) migrated to `Client` (`Has(Feature)` replaces the
      hand-rolled MINOR arithmetic; rode the item-3 migration, their 0d84f92).

---

## 5. Bridge request/result envelope — 🔨 implemented (2026-07-17)

**Problem.** Success/failure signaling is inconsistent per command
(`settings.set` acks; `settings.reset` replies data and *nothing* on failure;
`menu.open`/`hud.*`/`close` are silent even when they fail), `ui.error` has no
link to the offending message, and mod action buttons rely on a
`"<modId>.ack"` suffix-matching convention that exists only in the reference
view + mockbridge, not the typed contract. Retrofitting correlation after
views ship = breaking every view.

### Decisions

| Decision | Choice |
|---|---|
| Correlation | **Optional `requestId` + uniform `ui.result`.** Any `ui.command` may carry a caller-chosen `requestId`; every reply (`settings.ack`, `settings.data`, `ui.error`) echoes it; commands with no reply type today answer `ui.result {requestId, ok, code?, message?}` **when** a requestId was sent. Fire-and-forget stays available by omitting it — existing reference views keep working unchanged during the transition |
| Error shape | `ui.error` gains machine `code` (stable enum strings) + human `message` + echoed `command` + `requestId`. Existing `reason` kept through 0.x, removed at 1.0 |
| Mod actions | **Fold into the envelope** — the view sends the action command with a `requestId`; the plugin's reply surfaces as `ui.result` (ABI grows a reply path for command handlers, or the host wraps it). The `"<modId>.ack"` folklore dies before anyone ships against it |
| JS helper | **Ship `views/shared/osfui.js`** (loaded like `osfui.css`): `osfui.request(cmd, payload)` → Promise via generated requestId, `osfui.send()` fire-and-forget, `osfui.on(type, fn)` subscriptions — and nothing else (thin = freezable). Reference views migrate to it (becoming the copyable example); the harness mock loads the same file so it can't drift |

### Implementation checklist

- [x] `src/runtime/MessageBridge.cpp` — requestId plumbing: the id rides
      TOP-LEVEL in the envelope both directions (string 1–64 chars; longer/
      non-string = treated absent, warned); every reply sent through the
      no-target SendToWeb echoes it; a handler that produced no reply gets an
      automatic `ui.result { ok:true, command }`; `SendResult(ok, code,
      message)` for explicit outcomes (SILENT without a requestId — fire-and-
      forget keeps pre-0.5 behavior); `DeferResult()` + `CurrentRequestId()` +
      a 4-arg SendToWeb for deferred replies (the capture flow). `ui.error`
      reshape: `code` ("malformed-message"/"unknown-message-type"/
      "unknown-command") + `message` + legacy `reason` (dies at 1.0) +
      requestId echo where readable.
- [x] `src/runtime/Runtime.cpp` + `SettingsModule.cpp` — result codes:
      `menu.open`/`menu.close`/`hud.*` unknown id → `unknown-view` (already-
      open/closed = idempotent success via the auto-ack); `setViewHidden`
      unknown → `unknown-view`; `settings.reset` failure → `unknown-setting`
      (no longer silent), success now replies `settings.data` to the CALLER
      through the reply path (requestId echo — resolves the promise) and
      pushes to the other subscribers; `settings.captureKey` stashes the
      arming requestId and `settings.captured` echoes it (one request spans
      the whole rebind).
- [x] `src/api/BridgeApi.cpp` — **deviation: host-wraps-it, no ABI bump**
      (the design allowed either). The trampoline injects the caller's
      requestId INTO the payload JSON handed to the plugin (additive), and the
      bridge auto-ack answers `ui.result { ok:true }` = delivered-and-handled.
      A plugin publishing richer results uses its own message types (echoing
      the payload's requestId in its own payload for manual correlation); a
      first-class ABI reply vmethod can join a later MINOR if demanded.
- [x] `data/OSFUI/views/shared/osfui.js` — NEW, deliberately thin (frozen
      surface): decorates the injected `window.osfui` with `available()`,
      `ready` (promise of runtime.ready), `has()`, `send()`, `request()`
      (default 10 s timeout, `{timeoutMs:0}` disables; rejects with `.code`
      on ui.error / ui.result ok:false / timeout / no-bridge), `on()` →
      unsubscribe. The helper OWNS `onMessage`; correlated replies ALSO
      dispatch to `on()` subscribers (one render path). Reference views
      migrated: boot off `osfui.ready` + `has()`, set/reset/actions/capture
      via `request()`; the `"<modId>.ack"` folklore is deleted from the view
      AND the mock (actions resolve on `ui.result`).
- [x] Contract lockstep: `sdk/osfui.d.ts` (BridgeEnvelope.requestId,
      UiResultPayload, UiErrorPayload reshape, OSFUIHelper surface),
      `docs/authoring-views.md` (§3 rewritten helper-first),
      `docs/native-plugin-api.md` + `sdk/OSFUI_API.h` (CommandFn payload
      requestId + delivered semantics), `devtools/harness/mockbridge.js`
      (full envelope parity: rid threading, auto ui.result, error codes,
      capture-busy).
- [x] Protocol bump 0.4 → 0.5 (`Version.h`).
- [x] Tests: `bridge_api_tests` +35 checks (echo, auto-ack, fire-and-forget
      silence, over-long id, reply suppression, SendResult, DeferResult,
      ui.error codes, capabilities); `settings_module_tests` +ack/reset/
      requestId sections (mirrors runtime teardown order — OnBridgeDown
      before destructors, as the runtime does).

---

## 6. Capability-based feature detection — 🔨 implemented (2026-07-17)

*(Engineering design — no user forks; direction was set by items 2 and 5.)*

**Problem.** The documented version gate (`!bridgeVersion?.startsWith("0.")`)
is a no-op for the entire 0.x window; neither reference view gates at all;
`sdk/README.md` says protocol "0.1" while `Version.h` says "0.4". Views need a
way to detect features that isn't version arithmetic.

### Design

- `runtime.ready` gains **`capabilities: string[]`** — append-only named
  features. Naming: command-namespace names for surfaces (`settings`,
  `settings.captureKey`, `views`, `game.calendar`, `gamepad`), `request-id`
  for the item-5 envelope, `type:<t>` for setting value types
  (`type:flags`, …), `schema:requires` for the item-2 gate. The list only
  ever grows; a capability, once shipped, is never removed or renamed.
- Item 2's schema-level `requires` array consumes the **same names** — one
  vocabulary for JS views and settings schemas.
- The item-5 JS helper exposes it: `await osfui.ready` resolves with the
  payload; `osfui.has("type:flags")` is the documented gate. Reference views
  demonstrate it (they're the de-facto template — F5 from the audit).
- `bridgeVersion` stays, demoted to informational; docs rewrite the
  negotiation section capability-first, and the broken `startsWith("0.")`
  snippet is replaced everywhere it appears (`sdk/README.md`,
  `docs/authoring-views.md`).
- Mock parity: harness `runtime.ready` carries the same `capabilities` list
  (single-sourced into `mockbridge.js` — a mock that lies about capabilities
  defeats the whole mechanism).
- Version single-sourcing: CI check greps `kBridgeProtocolVersion` against
  the literals in `sdk/README.md` / docs so the "0.1 vs 0.4" drift class
  can't recur.

### Implementation checklist

- [x] `src/runtime/Capabilities.h` — the list hoisted to `Caps::kList`
      (namespace-scope array; `Has()` loops it) so the requires-gate and the
      handshake single-source; `"request-id"` added (ships with item 5).
      `MessageBridge::SendRuntimeReady` emits it as `capabilities` (rides the
      0.5 bump).
- [x] `shared/osfui.js` — `ready` promise + `has()` (false before ready).
- [x] Reference views demo it (`osfui.ready.then` boot; settings view gates
      `views.get` on `has("views")`); `mockbridge.js` mirrors the list
      (CAPABILITIES const, comment ties it to Capabilities.h) and now pushes
      `runtime.ready` proactively on install like the native runtime (the
      item-12 divergent-boot-semantics row, pulled forward — the helper's
      `ready` depends on it); `sdk/README.md` + `docs/authoring-views.md`
      negotiation sections rewritten capability-first, the broken
      `startsWith("0.")` snippet is gone everywhere.
- [x] CI doc-version grep: `.github/workflows/ci.yml` extracts
      `kBridgeProtocolVersion` and asserts the headline literals in
      `sdk/README.md`, `docs/authoring-views.md`, `sdk/osfui.d.ts` name the
      CURRENT version (historical "protocol 0.4" annotations deliberately
      not flagged).

---

## 7. File ownership: config, MCM, vanillakeys — 🔨 implemented (2026-07-17)

**Problem.** `config.json` and `vanillakeys.json` are user-edited files living
in the mod-owned folder (clobbered on every update); `toggleKey` and
`disableControls` exist in both `config.json` and the `osfui` MCM schema, and
the schema silently wins (the config edit does nothing after first boot).

### Decisions

| Decision | Choice |
|---|---|
| config.json role | **Developer/boot file, mod-owned, clobber-on-update is fine.** Holds backends, input source, diagnostic escape hatches (`hardwareCursor`, `focusMenu`, …), `view`/`views`, dev knobs. **No user-facing keys at all** |
| Split-brain | `toggleKey` and `disableControls` **removed from config.json** — the MCM schema is the sole owner. Schema defaults must equal the previously shipped config values (F10 / true) so fresh installs behave identically |
| Knob migration | **All four config-only user knobs move to the osfui schema now**: `focusKey` (key, default `Tab`) and `consoleKey` (key, default `Grave`) in the Input group — the layout-dependent keys the rebind UI exists for; `pauseMenuEntry` (bool, `requires:"reload"`) and `vanillaKeyConflicts` (bool) in an Interface group. Migrating later would silently strand users' config edits (lenient parser), so the set is settled pre-release |
| vanillakeys.json | Shipped file stays authoritative + **additive user overlay** `Documents/…/OSFUI/vanillakeys.user.json`: `{"add":[rows], "replace":[rows keyed by event], "suppress":["EventName"]}` merged at load. User fixes survive updates AND untouched rows keep receiving upstream corrections. Format-versioned (item 8) |

### Implementation checklist

- [x] `src/core/Config.{h,cpp}` — `toggleKey`/`disableControls`/`consoleKey`/
      `pauseMenuEntry`/`vanillaKeyConflicts` no longer PARSED; each present
      key logs INFO "now managed in Mod Settings" (one-release courtesy).
      **Deviation:** the struct FIELDS stay — they double as pre-replay boot
      defaults and as the runtime state `OnSettingChanged` mutates (so they
      must equal the schema defaults; documented in the header).
      **Deviation: `focusKey` DROPPED entirely, not migrated** — it was dead
      code (nothing consumed it; the Tab focus-cycle died with the
      single-menu stack). Its config key gets a dedicated removal INFO.
      Stale Tab-cycle claims scrubbed from troubleshooting.md.
- [x] `data/OSFUI/settings/osfui.json` — Input group grows `consoleKey`
      (type key, default Grave, `allowUnbound` — "" = pass-through off,
      `inputContext: "overlay"` with `blocksGameplay` so the definitional
      collision with @game Console doesn't badge); new Interface group:
      `disableControls`, `pauseMenuEntry`, `vanillaKeyConflicts` (defaults
      mirror the old shipped config: true/true/true).
      **Deviation: `pauseMenuEntry` ships WITHOUT `requires:"reload"`** — the
      Scaleform inject runs per pause-menu open behind a Tick gate, so the
      toggle is live by construction (applies on the next pause-menu open).
- [x] `src/runtime/Runtime.cpp` — `OnSettingChanged` cases: `consoleKey`
      re-resolves into the now-ATOMIC `_consoleKey` (read on the window
      thread); `disableControls` mutates live (ReconcileControlLayer already
      runs every tick with the release-on-disable shape); `pauseMenuEntry`
      mutates live; `vanillaKeyConflicts` → new `ApplyVanillaKeyConflicts`
      (lazy: table loads on first ENABLE — a persisted off never pays the
      parse; disable clears the store's table; either way the new
      `SettingsModule::BroadcastData()` re-syncs open views, since
      SetVanillaKeys bumps no generation). The old eager Initialize-time
      vanilla load is gone — the OnStart NotifyAll replay drives it.
- [x] `src/runtime/VanillaKeys.{h,cpp}` — `OverlayUserFile`: `add` (new rows),
      `replace` (rebind + optional relabel, case-insensitive event match),
      `suppress` (row removed — also leaves the keybinds full map);
      unknown events / unresolvable keys / duplicate adds WARN (typo
      diagnostics); applied AFTER the controlmap overlays (user's word is
      final). Discovery: `Documents/My Games/Starfield/OSFUI/
      vanillakeys.user.json`.
- [x] README config section rewritten (dev-file framing, `configVersion` row,
      moved-knob pointer, pauseMenuEntryLabel/View demoted to one dev row);
      troubleshooting.md gains "Where are my settings?" + the
      vanillakeys.user.json recipe; stale `OSF UI` values-path and "MOD
      SETTINGS" label references fixed.
- [ ] Keybinds view: badge user-overlaid vanilla rows — SKIPPED (nice-to-
      have, not release-gating; the runtime doesn't currently mark overlay
      provenance in the vanillaKeys table).
- [x] Tests: `vanilla_keys_tests` +16 checks (add/replace/suppress round
      trip, every typo diagnostic, malformed/missing overlay no-ops).

---

## 8. Format-version stamps + diagnostics — 🔨 implemented (2026-07-17)

*(Engineering design — no user forks.)*

**Problem.** No format-version field in `config.json`, view manifests,
`vanillakeys.json`, or the values-file encoding; a typo'd key silently
becomes a default with zero log output; no migration hook exists for renames.

### Design

- Stamps (all additive; lenient parsers ignore them on old builds):
  `config.json` → `"configVersion": 1`; `vanillakeys.json` +
  `vanillakeys.user.json` → `"formatVersion": 1`; values files → a
  `"$formatVersion": 1` meta key written on rewrite (invisible to the schema
  walk like `$schemaVersion`; item 2's preservation keeps it stable under
  future hosts). Manifests: optional `"manifestVersion"` accepted but not
  required — the nested layout (item 1) is itself the v2 discriminator.
- Migration hook: version greater than known → INFO "written by a newer OSF
  UI; unknown fields ignored" and continue leniently; less than known → run
  migrations (none exist yet; the hook is the point).
- Unknown-key diagnostics, scoped by who authors the file:
  - `config.json`, `vanillakeys*.json`: **WARN always** — they ship with (or
    override) the host, so there is no legitimate version-skew source of
    unknown keys; an unknown key is a typo.
  - Manifests + settings schemas: **devMode INFO only** — a newer mod on an
    older host makes unknown keys *normal* there (additive evolution);
    warning would spam exactly the compatible case items 1–2 designed for.

### Implementation checklist

- [x] Stamps + version-compare hooks: `config.json` `configVersion: 1`
      (`Config::kConfigVersion`; newer → INFO + lenient);
      `vanillakeys.json` + `vanillakeys.user.json` `formatVersion: 1`
      (`VanillaKeys::kFormatVersion`); values files `$formatVersion: 1`
      written on rewrite (`SparseValues` stamps `Mod::formatVersion` =
      max(known, loaded) so a NEWER host's stamp round-trips undowngraded;
      the load-time compare tolerates a missing stamp, so stamping alone
      NEVER dirties an existing file — the item-2 byte-identical clean-load
      guarantee holds, proven by test). Manifests: optional
      `manifestVersion` accepted (newer → INFO), not required.
      **Refinement:** `$`-prefixed keys are the reserved meta namespace —
      the two owned stamps are excluded from the preserved bag; any OTHER
      `$`-key (a future format's meta) is preserved verbatim like unknown
      settings.
- [x] `src/runtime/Json.{h,cpp}` — `ReportUnknownKeys(obj, known, source,
      warn)`: WARN for host-owned files (config.json, vanillakeys*.json — a
      typo), INFO for author files (callers gate on devMode: manifests +
      settings-schema top level). `$`-keys skipped (meta + editor
      $schema/$comment).
- [x] Docs: manifest.schema.json gains `manifestVersion` +
      `additionalProperties: true` (the item-12 row, pulled forward — item 8
      declares unknown manifest keys the NORMAL case, so the authoring
      schema must stop false-erroring); settings-schema.schema.json values
      path `OSF UI` → `OSFUI` fixed (another item-12 row) + protocol ref
      0.5; authoring-views notes the manifest leniency and the `$`-meta
      namespace; README documents `configVersion`.
- [x] Tests: `settings_store_tests` +$formatVersion section (stamped file
      loads clean; v7 stamp + foreign `$futureMeta` round-trip a rewrite);
      six exact-content assertions updated for the stamp-on-rewrite.

---

## 9. Shared CSS namespace + theming — 🔨 implemented (2026-07-17)

*(Engineering design — the schema-`accent` mechanism already won; this
formalizes it and cleans the namespace.)*

**Problem.** `views/shared/osfui.css` leaks collision-prone bare names into
every linking view (`.search-box`, `.close-btn`, the `.on` modifier), all
theme tokens are un-prefixed (`--accent`, `--line`, `--void-*`), and per-mod
theming exists twice: a hardcoded class enum
(`.osf-animation/.osf-seduce/.osf-defeat/.osf-body`) AND the schema `accent`
field the settings view already applies via `applyAccent` (derives all four
accent tokens from one hex, `main.js` ~839-862).

### Design

- **Tokens → `--osf-*`** (`--osf-accent`, `--osf-line`, `--osf-void-900`, …).
  Mechanical rename across shared css, both built-in views, harness, and the
  OSF Animation `osf` view (rides that repo's item-3 migration).
- **Classes: everything public is `osf-*`** — `.search-box` → `.osf-search`,
  `.close-btn` → `.osf-close`, bare `.on` → `.osf-on` (aria-pressed stays the
  primary state selector). After the rename, the rule is statable: *every
  class and custom property the kit exports carries the prefix; nothing
  un-prefixed is contract.*
- **Theme enum retired.** The per-mod classes are deleted; `.osf-ui`'s values
  become the `:root` defaults. Per-mod accent = the schema/manifest `accent`
  hex, full stop. The `applyAccent` derivation (hex → accent/hover/quiet/
  strong) moves into `shared/osfui.js` (item 5) as `osfui.applyAccent(el,
  hex)` so views and the settings host share one implementation, and the
  accent value is already surfaced per-mod in `settings.data`/`views.data`.
- **Element-level base styles stay global** (`a`, `kbd`, headings,
  `::selection`, scrollbars, form elements). Linking the stylesheet IS the
  opt-in — that's what a design-system base sheet is; scoping every selector
  under a root class would bloat the kit and break both shipped views for
  marginal benefit. Documented in authoring-views: link the kit for the
  native look, or don't link it and own your styling; there is no partial
  mode.

### Implementation checklist

- [x] `views/shared/osfui.css` — every exported token renamed `--osf-*`
      (~60 tokens, 278 refs in the kit alone); `.search-box` → `.osf-search`,
      `.close-btn` → `.osf-close`, bare `.on` companion → `.osf-on`
      (aria-pressed stays the primary state selector); theme-class enum
      DELETED (`.osf-ui/.osf-animation/.osf-seduce/.osf-defeat/.osf-body`) —
      `:root` is the OSF UI default; kit-contract statement in the header.
- [x] Built-in views + harness — mechanical rename (settings css 211 refs,
      keybinds css 104, JS + HTML class/markup sites, `body class="osf-ui"`
      dropped everywhere).
- [x] `shared/osfui.js` — `osfui.applyAccent(el, hex)` owns the one-hex →
      four-token derivation; the settings view's local copy is now a
      one-line delegate.
- [ ] OSF Animation view ride-along — **NOT NEEDED (deviation):** the
      browser view never linked the shared kit; it ships its own `:root`
      tokens under its own names, which the kit rule doesn't govern.
      Nothing to rename there.
- [x] `docs/authoring-views.md` — "The shared UI kit — contract" section
      (prefix rule, link-is-opt-in/all-or-nothing, accent mechanism).

---

## 10. Undocumented load-bearing messages — 🔨 implemented (2026-07-17)

*(Engineering design.)*

**Problem.** `ui.visibility`, `ui.gamepad`, and the `osfui.gamepadRaw` command
ship, are relied on (settings view scopes its undo baseline on
`ui.visibility`; the OSF scene browser switches camera modes on it), but are
absent from `sdk/osfui.d.ts` and the authoring docs. `gamepadRaw` also has an
implicit lifecycle (silently reset on overlay close, "re-assert on each show")
documented only in a C++ comment.

### Design

- **`ui.visibility {visible}` — promoted** to the typed contract (d.ts +
  authoring-views + mock emits it on the harness visibility toggle). It's
  simple and stable.
- **`ui.gamepad` — reshaped (item 11) then documented as `experimental`**
  through 0.x: gamepad navigation is explicitly "basic and being refined"
  (README), so the shape gets an instability stamp rather than a freeze. The
  `gamepad` capability (item 6) is the detection signal.
- **`osfui.gamepadRaw` — documented + lifecycle fixed**: raw mode becomes a
  sticky per-view property that survives overlay hide/show and clears only on
  view destroy — removing the undocumented "re-assert on every show"
  coupling. Experimental stamp alongside `ui.gamepad`.

### Implementation checklist

- [x] `src/runtime/Runtime.cpp` — gamepadRaw stickiness: per-view grant set
      (`_gamepadRawViews`); `DrainEngineInput` applies the ACTIVE view's flag
      each tick (a menu switch can't inherit another page's grant — stronger
      than the design asked); cleared on page (re)load (`OnViewLoad` — a
      fresh page starts un-granted and re-asserts in its own boot code) and
      on view destroy; the overlay-close reset is GONE. The command now
      answers `unknown-view` when it has no source view.
- [x] `sdk/osfui.d.ts` + `docs/authoring-views.md` — `ui.visibility`
      promoted (landed with slice 4's d.ts work; the stale
      "re-assert on show" prose removed now), experimental-through-0.x
      stamps on `ui.gamepad` + `osfui.gamepadRaw` with the `gamepad`
      capability as the detection signal, sticky lifecycle documented.
- [x] `devtools/harness/mockbridge.js` — `ui.visibility {visible:true}`
      pushed at install (after runtime.ready) +
      `window.osfui._mock.visibility(v)` for exercising hide edges.

---

## 11. Payload reshapes — 🔨 implemented (2026-07-17)

*(Engineering design — all breaking-of-0.x, riding the 0.5 bump with items
5–6 so views break once, not four times.)*

- **`game.data`**: calendar fields nest under `calendar: {available, day,
  month, year, hour, daysPassed}`; future providers add sibling objects
  instead of colliding at the root.
- **`ui.gamepad`**: one type, nested per-kind objects —
  `{kind:"button", button:{id, down}}` / `{kind:"stick", axes:{lx, ly, rx,
  ry}}`. Extensible: triggers → `axes.lt/rt`, second controller → a `pad`
  index field.
- **`settings.ack`**: gains `value` (the authoritative post-clamp value — an
  unsubscribed caller no longer has to re-fetch to learn what was stored) and
  `code`/`message` on failure (rejected vs clamped becomes distinguishable),
  aligned with the item-5 error shape.
- **`settings.changed`** on key-type settings carries the recomputed
  `conflicts` array — kills the documented full-registry re-fetch (N+1) the
  reference view currently bakes in.
- **Key capture**: correlation comes from item 5's `requestId`; a second arm
  while one is live is refused visibly (`ui.result ok:false,
  code:"capture-busy"`) instead of silently clobbering the first.
- Mock parity for every reshape.

### Implementation checklist

- [x] `src/runtime/Runtime.cpp` — `game.data` nests `calendar` (`available`
      INSIDE it); `ui.gamepad` nests `button:{id,down}` / `axes:{lx,ly,rx,ry}`;
      capture-busy: a second `settings.captureKey` while one is armed →
      `ui.result ok:false, code:"capture-busy"` (the first capture stands);
      capture correlation rides item 5 (`_captureRequestId`).
- [x] `SettingsModule.cpp` + `SettingsStore` — `settings.ack` gains `value`
      (authoritative post-clamp; clamp = SUCCESS + value, not a code) and
      `code`/`message` on failure via new `SettingsStore::SetWithResult`
      (codes: `unknown-setting`, `read-only` for stubs + unknown-typed
      settings, `invalid-value`); key-typed `settings.changed` ALWAYS carries
      `conflicts` (new `ConflictsForSetting` — `[]` = unique, the
      badge-clearing signal).
- [x] Reference views: settings view applies the pushed `conflicts` in place
      with a symmetric partner mirror (`applyConflictUpdate`) — the
      full-registry re-fetch on rebind (the N+1) is GONE; keybinds view needs
      no change (derives collisions from its own model). `mockbridge.js`
      mirrors every reshape (nested game.data sample, ack value/code,
      changed conflicts, capture-busy). `sdk/osfui.d.ts` +
      `docs/authoring-views.md` lockstep — the d.ts also gained
      `ui.visibility`/`ui.gamepad`/`osfui.gamepadRaw` typings (item 10's
      documentation rows, pulled forward since the shapes are now settled),
      and the mock's `views.data` emits `focused` on every entry + native
      reset parity (two more item-12 rows pulled forward).
- [x] Tests: `settings_store_tests` +SetWithResult codes +ConflictsForSetting
      (both-sides recompute, rebind-away clears); `settings_module_tests`
      +conflicts-on-changed (into/out of collision, non-key never carries).

---

## 12. Doc/contract drift sweep — 🔨 implemented (2026-07-17)

*(Mechanical checklist; several rows are owned by earlier items and listed
here only for completeness.)*

- [x] `sdk/README.md` — protocol version corrected + gate example rewritten
      capability-first (owned by item 6, which also adds the CI grep).
      *(Landed with slice 4.)*
- [x] `docs/schema/settings-schema.schema.json` — values path `OSF UI` →
      `OSFUI` (the documented path doesn't exist on disk). *(Landed with
      slice 5.)*
- [x] `docs/schema/manifest.schema.json` — `additionalProperties:false` →
      `true`: the runtime parser is lenient and item 8 declares unknown
      manifest keys the *normal* compatible case, so the authoring schema must
      stop flagging them (it already false-errored on the shipped `mod`
      field). *(Landed with slice 5's item 8.)*
- [x] `sdk/OSFUI_API.h` — reserved-prefix prose replaced by the item-3
      command-shape rule; stale protocol example strings; REX include guard,
      lifetime/threading contract lines (owned by item 4; the protocol
      strings finished with slice 4).
- [x] `sdk/osfui.d.ts` — gains `ui.visibility`/`ui.gamepad`/`gamepadRaw`
      (item 10), `requestId`/`ui.result` (item 5), `capabilities` (item 6),
      reshapes (item 11). *(Landed with the slice-4 lockstep.)*
- [x] `devtools/harness/mockbridge.js` — push `runtime.ready` proactively on
      install (native greets on load; the mock used to gate it behind a
      `views.get` call — divergent boot semantics); emit `focused` on every
      `views.data` entry (HUD fixtures omitted a field the d.ts marks
      required); `settings.reset` parity with native (suppress per-key
      `settings.changed`, reply one `settings.data`). *(All three landed with
      slice 4 — the helper's `ready` promise depends on the first.)*
- [x] `docs/authoring-views.md` — the focus model and the layout guarantee
      promoted to explicitly versioned guarantees (protocol 0.5).
      **Correction along the way:** the old prose promised a `Tab`
      focus-cycle that no longer exists (focusKey was dead code, dropped in
      slice 5) — the guaranteed model is "input goes to the top open menu;
      HUDs never receive input; no focus-cycle key". Troubleshooting's Tab
      claims were scrubbed in slice 5.

---

## Status & implementation order

**All 12 items designed as of 2026-07-17** (items 1–5, 7 via user Q&A; 6, 8,
9–12 engineering designs). **Items 1–4 implemented 2026-07-17**, including
the OSF Animation `osf` → `osf.animation` migration in the sibling repo
(both repos build green; all 8 native suites pass; nested layouts verified
in both MO2 deploys). **In-game verify pass for slices 1–3 passed 2026-07-17.**
**Slice 4 (items 5+6+11 — protocol 0.5) implemented 2026-07-17**: requestId/
`ui.result` envelope, capabilities handshake, payload reshapes, the
`shared/osfui.js` helper, reference-view + mock migration, CI version grep;
three item-12 rows pulled forward (mock boot semantics, `focused`, reset
parity) and item 10's d.ts documentation rows. Build green, all 8 suites pass
(store 209, module 69, bridge_api 74), harness pages render through the new
envelope. In-game verify for slice 4 pending. OSF Animation needs NO change
for 0.5 (its commands get delivered-acks automatically; its view assigns its
own `onMessage`, which is fine without the helper).
**Slice 5 (items 7+8) implemented 2026-07-17**: config.json demoted to a
dev/boot file (user knobs → the osfui schema; dead `focusKey` dropped),
vanillakeys user overlay, format stamps + unknown-key diagnostics
(+ two item-12 schema-doc rows pulled forward). Build green, all 8 suites
pass (store 215, vanilla 32).
**Slice 6 (items 9+10+12) implemented 2026-07-17 — ALL 12 ITEMS NOW
IMPLEMENTED.** Kit namespace (`--osf-*`/`osf-*`, theme enum deleted,
`osfui.applyAccent`; OSF Animation needed nothing — its view never linked
the kit), gamepadRaw sticky per view, experimental stamps on the gamepad
pair, focus/layout guarantees versioned, mock ui.visibility. Build green,
all 8 suites pass, harness renders pixel-faithful after the rename.
**Remaining: the in-game verify pass for slices 4–6**, then the freeze is
done and 1.0 packaging can proceed.
Dependency-ordered implementation slices:

1. **Item 1** — ID namespacing + nested layout (everything else keys off the
   id rules; includes the OSF Animation `osf` → `osf.animation` rename
   decided in item 3).
2. **Item 2** — settings forward-compat (preservation, `flags`, `requires`).
3. **Items 3 + 4** — the native-ABI slice (command shape, first-wins,
   wrapper-class SDK header, ABI 1.6, header hardening); one vendored-header
   update for OSF Animation.
4. **Items 5 + 6 + 11** — the protocol 0.5 slice (requestId/`ui.result`,
   capabilities, payload reshapes, `shared/osfui.js`, mock + reference-view
   migration) — bundled so views break once, not three times.
5. **Items 7 + 8** — file ownership + format stamps (7's knob migration uses
   2's key-type infra and 8's stamps).
6. **Items 9 + 10 + 12** — CSS namespace, message promotion, drift sweep
   (mechanical; parts ride earlier slices).

Each slice ends verify-first: build green + native tests + harness pass, and
the in-game checks called out per item, before the next slice starts.

### Already solid (keep as-is)

Sparse user-values persistence with schema-default tracking; `aliases` rename
mechanism; uniformly lenient JSON parsing (`src/runtime/Json.cpp`); the
no-structs/no-enums ABI discipline; README's honest "0.x and unstable" framing.
