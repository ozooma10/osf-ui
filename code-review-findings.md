# OSF UI — MCM settings renderer: code review findings

**Branch:** `feat/mcm-settings-renderer` (branch diff vs `main` + uncommitted working tree)
**Date:** 2026-07-13
**Method:** 10 independent finder angles across the full diff, deduped; load-bearing claims verified against source (`Runtime.cpp`, `SettingsStore.cpp`, `SettingsModule.cpp`). Build/run was **not** exercised — native tests need the Windows/xmake build.
**Verdict key:** CONFIRMED = inputs/state + wrong result identified at source; PLAUSIBLE = mechanism real, trigger depends on schema-author choices.

No `CLAUDE.md` governs the changed paths, so house-convention checks were skipped.

---

## Launch-blockers

### 1. Unsanitized mod `id` → arbitrary file write (security) · CONFIRMED
[src/runtime/SettingsStore.cpp:117](src/runtime/SettingsStore.cpp#L117)

`mod.valuesPath = _valuesDir / (mod.id + ".json")` with no sanitization. A schema `id`
containing `..` or a path separator escapes the settings directory.

**Failure:** A drop-in `settings/x.json` with `"id": "..\\..\\Starfield"` (the id field
overrides the filename stem at line 91), or a `RegisterSchema` with such an id, sets
`valuesPath` outside `Documents\...\OSFUI\settings`. The first `settings.set` calls
`Persist`, which `create_directories` + writes the file at an attacker-chosen location.

**Fix:** Validate/reject ids to a safe charset (e.g. `[A-Za-z0-9._-]`, no `..`) at the
store boundary in `AddSchema`.

### 2. Schema `id` hijack — id-must-equal-stem promised but not enforced · CONFIRMED
[src/runtime/SettingsStore.cpp:91](src/runtime/SettingsStore.cpp#L91)

`AddSchema` takes the schema's `"id"` field over the filename stem, but
`docs/schema/settings-schema.schema.json` and `mcm-design.md` promise `id == stem` with
warn-and-override. No enforcement exists; combined with the new sorted first-wins loading,
hijack is deterministic.

**Failure:** A mod ships `settings/aaa.json` with `"id": "osfui"`. Files load sorted by
filename, so `aaa.json` registers as `osfui` first and the real `osfui.json` is dropped as
a duplicate. The framework's (or a victim mod's) settings card is silently replaced and its
values file is driven by the impostor schema. MO2's per-file VFS priority never sees the
conflict because the filenames differ.

**Fix:** Enforce `id == stem` for drop-ins (override to stem + warn, as documented). Shares
the sanitization fix with #1.

### 3. Renderer ignores `settings.changed` → stale menu + no reconciliation · CONFIRMED
[data/OSFUI/views/settings/main.js:975](data/OSFUI/views/settings/main.js#L975)

`onNativeMessage` has no case for the new native `settings.changed` push that the
uncommitted `SettingsModule` change was built to feed. It falls to the default branch
(only `*.ack` handled) and is dropped.

**Failure:** The menu is open and subscribed. A sibling DLL, a mod HUD, or a preset commits
a value native clamps (e.g. preset `opacity: 2.0` clamped to max `1.0`; `Set` returns
`ok:true`). Native pushes `settings.changed` with the clamped value; the renderer discards
it. The visible control, modified-dots, rail counts, condition evaluation, and the
session-revert baseline all keep running on a phantom value the store never held, until the
menu is reopened. The optimistic local model never reconciles with native clamping.

**Fix:** Handle `settings.changed { mod, key, value }` — update `allMods`, refresh live
state. This one handler also resolves finding #11 (reset leaves other views stale).

### 4. `makeSettingRow` TypeError blanks the whole detail pane · CONFIRMED
[data/OSFUI/views/settings/main.js:390](data/OSFUI/views/settings/main.js#L390)

`(setting.label || setting.key).toLowerCase()` with no `|| ""` guard, unlike `railMatches`
/`renderSearch` which were hardened in this diff.

**Failure:** A drop-in schema contains `{ "type": "bool" }` (no key) in a group. The store
keeps the schema verbatim in `DataJson`, `buildBool` succeeds, then
`row.dataset.label = (undefined).toLowerCase()` throws inside `renderDetail`, leaving that
mod's entire settings pane empty with no error surfaced.

**Fix:** `(setting.label || setting.key || "")` here too; ideally skip settings with no key.

### 5. Every `type:"key"` rebind dead-ends in-game · CONFIRMED
[data/OSFUI/views/settings/main.js:1063](data/OSFUI/views/settings/main.js#L1063) · [src/runtime/Runtime.cpp:967](src/runtime/Runtime.cpp#L967)

The renderer arms native capture for any `type:"key"` setting and the shipped example
advertises rebindable mod keys, but `Runtime.cpp:967` still rejects everything except
`osfui.toggleKey` with no reply.

**Failure:** A user installs `examples/settings-only/mymod.json` (`hud.toggleKey`, hint
"Press to rebind") and clicks rebind in-game. Runtime logs a WARN and never replies; the
button shows "Press a key…" for 5s, then the self-heal toast "Rebinding this key isn't
available yet." The dev harness's generalized mock capture hides this, so authors ship
schemas verified against behavior the runtime refuses.

**Fix:** Gate native capture on `SettingsStore::GetSettingType(...) == "key"` (the getter
this branch added for exactly this) instead of the hardcoded allowlist; stop hardcoding
`mod:"osfui"` in `DrainKeyCapture`.

---

## High severity (functional bugs, not blockers)

### 6. Action-button timeout stale closure false-cancels a re-click · CONFIRMED
[data/OSFUI/views/settings/main.js:592](data/OSFUI/views/settings/main.js#L592)

The 5s timeout restore checks only `pendingActions.has(pkey)`, not whether the pending
entry is its own.

**Failure:** Click action (t=0); mod acks at t=1, entry deleted; click again at t=2 (new
entry, same pkey). The first timer fires at t=5, sees the key present, deletes the second
run's entry, re-enables the button, and toasts "No response from <mod>" only 3s into the
second run. The real ack at t=6 no-ops — a false timeout on an action that succeeded.

**Fix:** Store a token/identity per fire and have the timer compare it before restoring.

### 7. `buildColor` reverts to stale value on invalid input · CONFIRMED
[data/OSFUI/views/settings/main.js:341](data/OSFUI/views/settings/main.js#L341)

`current` is only updated by preset clicks, not by a successful hex commit, so the
invalid-input revert restores the session-start value, not the last committed colour.

**Failure:** Colour starts `#5aa9b8`; user types `#112233` and blurs (committed); later types
`oops` and blurs → field/swatch reset to `#5aa9b8` while the stored value is `#112233`. UI
and store silently disagree until a full re-render.

**Fix:** Update `current` on each successful commit.

### 8. Per-mod accent color leaks onto other mods · CONFIRMED
[data/OSFUI/views/settings/main.js:741](data/OSFUI/views/settings/main.js#L741)

`applyAccent` sets `--accent` on the persistent `detailEl` when a schema has a valid accent
but never clears it.

**Failure:** Select the demo mod (accent `#e6904a`), then select osfui (no accent).
`renderDetail` clears children but `detailEl.style` still carries `--accent:#e6904a`, so the
osfui pane's dots, badges, focus rings, and buttons render orange until a mod with its own
accent is selected.

**Fix:** Clear (or reset to default) the custom property when the schema has no valid accent.

### 9. `settings.reset` leaves other subscribed views stale · CONFIRMED
[src/runtime/SettingsModule.cpp:82](src/runtime/SettingsModule.cpp#L82)

The handler re-sends authoritative `settings.data` only to the calling view; others are
expected to sync via `settings.changed`, which the renderer doesn't handle (#3).

**Failure:** Two views subscribed via `settings.get` (settings menu + a mod's own panel). The
panel sends `settings.reset` for mod X; only the panel gets `settings.data`. The menu gets
per-key `settings.changed` it ignores and keeps showing pre-reset values.

**Fix:** Resolved by #3, or re-broadcast the registry to all subscribers on reset.

### 10. `SettingsModule::_subscribers` never pruned on view destroy · CONFIRMED
[src/runtime/SettingsModule.cpp:72](src/runtime/SettingsModule.cpp#L72)

Copies the subscribe-on-read pattern but not its cleanup: Runtime erases destroyed views
from `_viewsSubscribers` ([Runtime.cpp:482](src/runtime/Runtime.cpp#L482)) but nothing
removes them from `SettingsModule::_subscribers` except full bridge teardown.

**Failure:** A subscribed view exhausts its crash-recovery reloads and Runtime destroys it,
pruning `_viewsSubscribers`. Its id stays in `_subscribers` for the process lifetime, so
every later `Set`/`Reset`/`RegisterSchema` loops `PushToSubscribers` over a dead id. The set
only grows.

**Fix:** Hook the same destroy-time pruning, ideally a shared subscriber facility (see
"Altitude" below).

---

## Medium severity

### 11. Stepper `"step": 0` → NaN committed over the bridge · CONFIRMED
[data/OSFUI/views/settings/main.js:258](data/OSFUI/views/settings/main.js#L258)

`snap` divides by the schema-supplied step with no zero guard.

**Failure:** `{ type:'int', min:0, max:10, step:0, widget:'stepper' }`. Clicking +/- computes
`(v-min)/0` → `NaN` → readout shows "NaN", commit sends `NaN` → `JSON.stringify` → `null` →
native rejects → toast + full `settings.get` re-pull on every click.

**Fix:** Guard `step <= 0` (fall back to a sane default).

### 12. Action-command namespace check allows reserved-prefix ids (security) · PLAUSIBLE
[data/OSFUI/views/settings/main.js:585](data/OSFUI/views/settings/main.js#L585)

The check only requires `command.startsWith(mod.id + ".")`, so a schema whose id is a
reserved framework prefix (`menu`/`hud`/`ui`/`settings`) can fire framework commands.

**Failure:** A drop-in `settings/menu.json` (id `menu`) with `{ command: 'menu.close' }`
passes and sends the framework `menu.close`. The harness MockBridge excludes
`ui.`/`settings.`/`menu.`/`hud.` prefixes; the shipped renderer does not, and the store
reserves no ids.

**Fix:** Reserve framework id prefixes at the store; or block those command namespaces in the
action fire path.

### 13. `safeAssetSrc` doesn't validate the mod id it interpolates (security) · PLAUSIBLE
[data/OSFUI/views/settings/main.js:546](data/OSFUI/views/settings/main.js#L546)

Validates the `src` lexically but interpolates the id into `../${modId}/${s}` unchecked.

**Failure:** A schema with `"id": ".."` and `{src:'x.png'}` produces `../../x.png`, escaping
the `views/<id>/` confinement despite the src passing its own check. Same unsanitized-id root
cause as #1/#2, different surface.

**Fix:** Shared id sanitization (#1/#2).

### 14. Session tracking breaks if a mod id contains a space · PLAUSIBLE
[data/OSFUI/views/settings/main.js:215](data/OSFUI/views/settings/main.js#L215)

Baseline keys join `"<modId> <key>"` and `splitBKey` splits on the first space, but ids
derive from filename stems / schema strings that can legally contain spaces.

**Failure:** A mod ships `My Mod.json` (id `My Mod`). Changing a setting produces baseline key
`My Mod hud.scale`, parsed as modId `My`. `sessionChangeCount`/`openSessionPanel` find no mod
`My`, so the chip never counts it and Revert can't target it. (The code comment concedes keys
"theoretically could" contain a space.)

**Fix:** Use a nested `baseline[modId][key]` structure, or enforce an id charset (ties to #2).

---

## Efficiency (real cost, lower priority)

### 15. `settings.reset` double-delivers; push payloads built with no subscribers · CONFIRMED
[src/runtime/SettingsModule.cpp:26](src/runtime/SettingsModule.cpp#L26)

`reset` fires one `settings.changed` per key (from the change listener) *and* a full
`settings.data`; the changed-payload json/strings are built before `PushToSubscribers`
checks for a bridge/subscribers.

**Failure:** "Reset all" on a 50-setting mod sends 51 bridge messages (50 redundant
`settings.changed` the renderer ignores + 1 `settings.data`), each serialized natively and
parsed in JS — a main-thread stall that grows with schema size. Separately, startup
`NotifyAll` and every set with the overlay closed still allocate the changed-payload before
the early-return: discarded work on the game thread.

**Fix:** Move the `_bridge`/`_subscribers.empty()` guard ahead of payload construction;
suppress per-key pushes when a full re-broadcast follows.

### Other efficiency notes (from finders, not individually verified)
- **Preset apply = N disk writes.** `applyPreset` fans out N independent `settings.set`, each
  doing a synchronous full-values-file `Persist` (tmp+rename) on the game thread — an IO storm
  per click. Consider a batched `settings.setMany` / debounced write-behind.
- **`refreshLive` is O(mods × settings) per commit.** Runs on every toggle/stepper/slider
  release and rescans every mod via `updateRailCounts` + `modifiedCount`; `sessionChangeCount`
  adds another linear pass. Recompute only the committed mod; index mods by id in a `Map`.
- **Filter input has no debounce.** Every keystroke runs `renderRail` + `renderDetail`
  (full `renderSearch` cross-mod scan + pane rebuild). Debounce ~100ms.
- **`DataJson()` serialize→reparse round-trip**, now at 3 call sites in `SettingsModule.cpp`.
  A `SettingsStore::Data()` returning the json object avoids dump-then-parse.

---

## Cleanup / simplification / altitude (quality, not correctness)

- **Duplicated subscriber plumbing.** `SettingsModule`'s bridge-pointer + subscriber-set +
  clear-on-`RegisterCommands` + `OnBridgeDown` re-implements Runtime's `_viewsSubscribers`
  pattern (minus the destroy-time pruning — see #10). A bridge-level subscription facility with
  eviction is the right depth before the planned HotkeyService makes it a third copy.
- **Duplicated rail-badge logic.** `updateRailCounts` re-creates `.rail-item-count` spans that
  `railItem` already builds — and has already drifted (the live-update path drops the
  `"N changed from default"` title tooltip).
- **Warning-callout CSS duplicated.** `.banner`/`.banner--warn` (settings/style.css) re-does
  `.osf-note`/`.osf-note--warn` (shared/osfui.css) with divergent padding/background.
- **`domKeyName` duplicated** between the shipped view and `mockbridge.js`, already drifted
  (harness copy drops Backspace/Insert/Delete/Home/End/PageUp/PageDown).
- **`isSetting` type list duplicated 3×** (main.js `isSetting`, the `buildSettingControl`
  switch, mockbridge.js) — adding the planned `color`/`flags` types means editing all three,
  and missing one silently excludes the type from search/modified-count/reset.
- **Two "registry shape changed" mechanisms** — the `_generation` counter (read only by tests)
  and the new `RegistryListener` callbacks. Every mutation site must remember both; `RemoveMod`
  originally bumped generation with no notification until the working diff patched it.
- **`maxLength` / 256-char cap not natively enforced.** The renderer clamps but the store
  ignores per-setting `maxLength` (hard `kMaxStringLen=256`), and the constant is hand-copied
  into three files with "bump in lockstep" comments. Any non-UI writer (preset, ABI, Papyrus)
  can exceed the advertised per-setting limit.
- **`color` widget has no native validation.** Nothing at the store boundary guarantees a
  parseable `#rrggbb`; a preset or non-UI writer can commit `blue` into a `widget:"color"`
  setting. Design doc defers `type:"color"` to the native slice, but the example template ships
  color settings today.
- **Duplicate control ids.** `ctl-<key>` has no mod prefix; the same key in two groups yields
  duplicate DOM ids, breaking `label[for]` association.
- **Search-jump into a collapsed group** lands on an invisible row — the search-result click
  handler doesn't un-collapse the target group (only the section index does).
- **Mock bridge never emits `settings.changed`**, so the harness can't test the documented
  push contract that the SDK `.d.ts` advertises.
- **`COLOR_PRESETS` / `rgba(47,53,61,.5)` literals** duplicate shared design tokens; a palette
  change updates one place and not the other.

---

## Method notes
- Reviewed for defects; **no build or run** was performed. Native `SettingsStore` tests require
  the Windows/xmake build.
- The `settings.changed` handler (#3) is the single highest-leverage fix: it also closes #9 and
  makes the optimistic model reconcile with native clamping.
- The id-sanitization fix (#1/#2) also closes #13 and de-risks #14.
