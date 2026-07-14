# Mod Configuration Menu (MCM) platform — design / RFC

**Status:** Proposed 2026-07-03 · not yet implemented · **Brand:** OSF UI ·
**Prior art:** Skyrim MCM (SkyUI) + MCM Helper, studied as lessons, not as a
compatibility target.

This document designs the settings system's growth into a **platform**: any
external mod — an SFSE DLL, an esm+Papyrus mod, or a pure-web view — registers
a section in the OSF UI settings menu, drives typed settings from it, and
*consumes* those settings where its logic lives. Skyrim's MCM was
settings-only because Flash/Scaleform made rich UI painful; OSF UI renders
real web pages, so the bar here is a system that feels next-generation rather
than an MCM port — while keeping the simplest tier at **zero code**.

Everything below builds on the existing schema-driven settings system
(`SettingsStore` + the `settings` view) and the native plugin bridge API
(`docs/native-plugin-api.md`). Nothing replaces them.

---

## 1. Summary — the three-tier authoring ladder

The design stance is a ladder of three tiers, each an obvious on-ramp to the
next, all discoverable from the same settings card:

| Tier | Effort | What the author ships | Covers |
|---|---|---|---|
| **1 — Schema only** | zero code | `settings/<id>.json` | ~80% of mods: typed knobs, groups, conditions, actions, presets |
| **2 — Schema + panel** | a little JS | schema `"panel"` entry + `views/<id>/mcm-panel.js` | previews, graphs, custom editors *inside* the mod's settings page |
| **3 — Full view** | own view folder | `views/<id>/` + schema `"launch"` entry | dashboards, browsers, anything — the full web platform |

Two principles hold across every tier:

> **`SettingsStore` is the single source of truth.** Every surface — the
> settings view, the C ABI, Papyrus, web push — is a projection over it.
> Nothing else validates, persists, or notifies.

> **One schema format for every tier.** The registration payload is always
> the same settings JSON document — a file on disk (Tier 0 drop-in) or a
> `const char*` over the C ABI. No parallel struct-based format.

And one lesson from MCM Helper drives the priorities: **consumption is the
product**. Registration is already nearly free today (drop a JSON file); MCM
Helper won Skyrim because *reading* a setting was one line of Papyrus. The
investment below goes into getters and change delivery for native, Papyrus,
and web consumers.

---

## 2. Goals / non-goals

**Goals**

- Zero-code settings registration stays the flagship path, hardened for the
  real ecosystem (MO2 VFS collisions, schema updates, localization).
- Sibling SFSE DLLs register schemas at runtime and subscribe to changes over
  the existing ABI-stable bridge (`sdk/OSFUI_API.h`, appended as MINOR 1.2).
- Papyrus-only mods (esm + scripts, no DLL) can **read and write** their
  settings via native functions (`OSFUI.psc`) — a later milestone, but a
  committed one.
- Any schema `key`-type setting is rebindable (today only `osfui.toggleKey`
  is), with informational conflict surfacing and a central **HotkeyService**
  dispatching presses to native, web, and (later) Papyrus subscribers.
- The settings view gains platform-level UX no per-mod system could build:
  global search, modified indicators, author presets, session revert,
  restart-required aggregation.
- A middle tier for "more interesting UI": a mod-supplied JS panel mounted
  inside its own settings page, behind a scoped API.

**Non-goals**

- **No Papyrus registration API.** Drop-in JSON already gives esm mods
  zero-code registration — strictly better ergonomics than assembling JSON
  strings in Papyrus. Papyrus gets consumption natives only (§8.4).
- **No INI mirroring** (MCM Helper's crutch). It existed because SkyUI had no
  native read path; we own the natives, and a second on-disk copy violates
  single-source-of-truth. (The Documents values JSON is trivially readable by
  tooling anyway; it is never documented as API.)
- **No string expression language** for conditions (parser, injection
  surface, versioning pain — see §4.1).
- **No iframe or composited-slot panels** (§6.1 analyzes why).
- No transactional apply/cancel semantics; apply-immediately with live native
  reaction is a strength, not a bug (§5).

---

## 3. Background: what exists today

All real and in-tree (verified 2026-07-03):

| Concern | Where | Note |
|---|---|---|
| Schema registry | `SettingsStore::LoadAll` `runtime/SettingsStore.cpp:37-91` | scans `settings/*.json` once at SFSE `kLoad`; no incremental add |
| Validation boundary | `SettingsStore::Validate` / `Set` `SettingsStore.cpp:194-245` | writes only touch schema-declared keys, clamped to type/range |
| Persistence | per-mod `Documents\My Games\Starfield\OSFUI\settings\<id>.json`, atomic tmp+rename `SettingsStore.cpp:288-310` | currently writes **all** values once any changes (see §11) |
| Change delivery | single `ChangeListener` slot `SettingsStore.h:33,39` | consumed by `Runtime::OnSettingChanged` `Runtime.cpp:1010` — framework knobs only; other mods have **no reaction path** |
| Bridge commands | `settings.get/set/reset` `runtime/SettingsModule.cpp:25-47` | `settings.get` replies `settings.data`; no change push to views |
| Renderer | `views/settings/main.js` | pure schema renderer; master/detail; `buildControl()` maps type→widget |
| Rebinding | `settings.captureKey` `Runtime.cpp:960-971` | **hardcoded allowlist `osfui.toggleKey`**; reply hardcodes `{"mod","osfui"}` in `DrainKeyCapture` `Runtime.cpp:884-885`. The web side is already generic (sends and consumes `{mod,key}`) |
| Key mapping | `ResolveKeyName`/`KeyName` `input/InputRouter.cpp:9-106` | name↔VK both directions |
| Native plugin ABI | `sdk/OSFUI_API.h` v1.1, append-only vtable | commands + SendToWeb + RequestMenu; registrations survive bridge re-creation; all effects land on the main thread via the pump |
| Subscribe-on-read precedent | `views.get` → `_viewsSubscribers` `Runtime.cpp:950-955` | the pattern `settings.changed` push copies |
| View sandbox | `docs/security-model.md` rule 3 | a view may read **sibling** view folders' assets — the mechanism Tier-2 panels reuse |
| Formal contracts | `docs/schema/settings-schema.schema.json` + `sdk/osfui.d.ts` | kept in lockstep with the renderer — this discipline continues for every addition below |

**Load-order timing fact that shapes §8/§10:** `Runtime::Initialize` (and thus
`LoadAll`) runs at SFSE `kLoad` (`core/Plugin.cpp`); sibling DLLs fetch the
bridge at `kPostLoad`+. Runtime schema registration therefore always lands
*after* the drop-in scan — the store must support incremental adds.

---

## 4. Tier 1 — schema language power-ups

Layering rule: **presentation-only features live in the host renderer and need
no native change; anything that changes what values are legal must also land
in `SettingsStore::Validate`.** The store is the security boundary; the
renderer is convenience. Each item is tagged `[renderer]` or
`[renderer+native]`.

### 4.1 Conditional visibility / enablement — `[renderer]`

The highest-value zero-code feature (MCM authors hacked this with page
rebuilds). A JSON **condition object** — structurally validated, referencing
sibling keys of the same mod only:

```jsonc
{ "key": "compassSize", "type": "float", "min": 0.5, "max": 2.0, "default": 1.0,
  "visibleWhen": { "key": "compass.enabled", "eq": true },
  "enabledWhen": { "all": [
    { "key": "mode", "in": ["compact", "full"] },
    { "not": { "key": "scale", "lt": 75 } }
  ] } }
```

Operators `eq ne in gt gte lt lte truthy`; combinators `all any not`. Allowed
on settings **and groups**. Semantics stated plainly in authoring docs: this
is *display sugar* — a hidden setting is still writable via the bridge and
still natively validated; a condition referencing an unknown key evaluates
false (dev-mode warning). Re-evaluated on every `settings.set` echo by
toggling row classes — a localized repaint, friendly to the dirty-rect
renderer; never a full re-render.

### 4.2 Widget hints — `[renderer]`

A `"widget"` field on existing types. Older runtimes ignore it safely.

- `enum` + `"widget": "segmented"` → radio segmented control (right for 2–4
  options; dropdown stays right for 5+). Plus `"optionLabels"` so stored
  values stay machine-stable while display strings are human:
  `{ "options": ["off","min","full"], "optionLabels": ["Off","Minimal","Full"] }`
- `int`/`float` + `"widget": "stepper"` → −/+ stepper with editable number.
- `string` + `"widget": "textarea"` + optional `"maxLength"` — may raise the
  limit per-setting up to a hard ceiling of 4096 (default 256 when
  unspecified; decided, §14.8).
- Slider **unit formatting**: `"format": { "suffix": "%", "scale": 100,
  "decimals": 0 }` — store `0.0–1.0`, display `85%`. Kills the single ugliest
  MCM pattern (raw floats in the UI).

### 4.3 New value types — `[renderer+native]`

- `"type": "color"` — stored `"#rrggbb"` or `"#rrggbbaa"`; `Validate` adds a
  regex check. UI is a swatch-grid + hex field (90% of the value at 10% of
  the repaint cost of a full HSV picker).
- `"type": "flags"` — multi-select over `options`, stored as an array of
  option strings, natively validated as subset-of-options. **v1.5** — real
  demand exists ("which slots does this apply to") but color ships first.

### 4.4 Action buttons — `[renderer]`, the sleeper feature

What turns a settings page from a form into a *control panel* — and the
native plumbing already exists (bridge command registration):

```jsonc
{ "type": "action", "key": "recalibrate", "label": "Run calibration",
  "command": "mymod.recalibrate",
  "hint": "Re-samples your current loadout.",
  "style": "default",                       // "default" | "accent" | "danger"
  "confirm": "This resets learned data. Continue?",   // optional modal
  "enabledWhen": { "key": "enabled", "eq": true } }
```

Host enforcement (critical): the renderer **only fires commands prefixed
`<modId> + "."`** — a schema cannot fire `settings.reset` on another mod or
probe reserved prefixes. (Native registration already refuses
`ui./runtime./game./settings./views.` — defense stays layered.) The button
enters a pending state and resolves on the mod's conventional `<modid>.ack`
reply or a 5 s timeout, surfaced as a toast. The documented pairing is:
*schema button ⇄ `RegisterCommand` in the mod's own SFSE plugin.* (A future
Papyrus event emitter for actions is possible but not designed here.)

### 4.5 Static content blocks — `[renderer]`

```jsonc
{ "type": "note", "text": "Requires **Shattered Space**. See the *tuning guide*.", "style": "info" }
{ "type": "image", "src": "assets/preview.png", "caption": "Compact layout", "height": 120 }
```

Injection-safety bar stays where it is today: the "markdown" is a
**micro-subset** (`**bold**`, `*italic*`, `\n`) implemented as a hand-rolled
tokenizer emitting `createElement`/`textContent` nodes — never `innerHTML`,
no links, no arbitrary HTML. `image.src` is host-resolved to
`../<modViewFolder>/<src>` with a lexical check (reject `..`, absolute paths,
URL schemes) *before* the sandbox's own check. Images require the mod to ship
a `views/<id>/` folder (a manifest-only folder is fine) — acceptable, and it
nudges authors up the ladder.

### 4.6 Requires-restart badges — `[renderer]`

```jsonc
{ "key": "renderer.backend", "type": "enum", "options": ["d3d12","gdi"],
  "requires": "restart" }        // "restart" | "reload" | "newGame"
```

Row renders a badge; the host aggregates: after any change to a
`requires:"restart"` setting, a dismissable banner pins to the detail pane
("1 change takes effect after restart"). Pure presentation, large trust win.

### 4.7 Deferred / rejected

- **`pages[]` tabs — v2.** Schema stays flat (`groups[]`). v1 mitigation:
  with >4 groups the host auto-renders a sticky section index from group
  labels, and groups accept `"collapsed": true`. Real pages are additive
  later; nothing is painted into a corner.
- **Rejected:** file/path pickers (views have no filesystem; an honest string
  + hint instead), date/time types, nested objects, cross-mod dependency
  warnings (except hotkeys, §9).

**Forward-compat policy (unknown schema features):** a renderer/store build
older than a schema must degrade, never crash or half-work. Unknown
*decorations* (`widget`, `format`, `requires`, conditions) are ignored — the
setting renders plainly. An unknown *type* renders as a read-only row with
the raw value and a "requires a newer OSF UI" hint, and the store refuses
writes to it (no clamping rules ⇒ not writable). This rule ships in v1 so
every later addition is automatically safe to adopt.

### 4.8 Localization

Localization is designed around the ecosystem's real shape: **translation
mods are pure data mods** — third parties ship a single language's strings
for a mod they don't own (Skyrim's `Interface/Translations/<mod>_<lang>.txt`
made this a thriving convention). That decides the layout: **one file per
mod per language**, so a translation is one drop-in file resolved by MO2
priority like any other conflict.

- **Convention:** any user-visible schema string starting `$` is a lookup
  key. Localizable fields: `title`, `description`, group `label`, setting
  `label`/`hint`, `optionLabels` entries, `note` text/caption, `action`
  label/`confirm`, preset `label`/`description`. Never localized: `id`,
  `key`, `options` values (machine-stable), commands.
- **Files:** `data/OSFUI/settings/l10n/<id>_<lang>.json` — flat
  `{ "$KEY": "text" }`, UTF-8 JSON (not Skyrim's UTF-16 `.txt`; JSON matches
  the rest of the system and tooling is trivial). The author ships
  `mymod_en.json`; a translation mod ships just `mymod_de.json` — add *or*
  override, zero code, and two competing translations conflict at file level
  where MO2 already arbitrates.
- **Resolution: native, at `DataJson()` time** — every consumer (settings
  view, panels, Papyrus display, alternate skins) receives resolved text;
  untrusted JS never performs lookups. Fallback is **per-key**: active
  language → `en` → literal key minus `$`. Per-key merge means a partial
  translation degrades to English, not to raw keys — essential because
  translation mods lag schema updates by design.
- **Language source:** `osfui.language` setting, default `"auto"` (reads the
  game's `sLanguage` INI). Changing it re-resolves, bumps the registry
  generation, and re-broadcasts `settings.data` — live language switch for
  free via the existing refresh path (§8.5).
- **Host chrome included:** the settings view's own strings ("Search",
  "Reset", "MODS", banner text) localize through the same mechanism under
  the `osfui` id — translators localize the framework exactly like any mod.
- **Tier 2/3 text:** a mod's resolved string table rides its `settings.data`
  entry; panels read it via `ctx.i18n(key)`. Full custom views can request
  `i18n.get {mod}` (sketch — finalize with the panel API).
- **Translator DX:** the dev harness accepts `?lang=`; devMode hot-reload
  (§12) watches `l10n/` too; `osfui lint` (v2) reports missing/orphaned keys
  by diffing a language file against the schema's `$KEY` set.
- **Font coverage is the honest risk:** Ultralight renders with the fonts we
  give it — CJK/Cyrillic coverage does not come free. Before advertising
  localization, verify the font fallback stack for non-Latin scripts and
  decide whether to bundle a subset (e.g. Noto) — open question §14.

Rollout: v1 documents the convention and renders `$KEY` minus `$` as the
fallback (schemas written today localize later without edits); loading and
resolution land in **M2** — translation mods are an ecosystem-adoption
lever, not a nicety, so this deliberately does not wait for M3.

---

## 5. Platform-level UX — the "next-gen, not a port" argument

Implemented once in the host settings view; every mod benefits. This is what
Scaleform MCM could never do.

**v1**

1. **Modified indicators + per-setting reset.** The host compares
   `values[key] !== default` (defaults already ship in the payload); modified
   rows get an accent dot, hover reveals a per-row reset glyph (native
   `settings.reset {mod,key}` already supports it — the UI just never exposed
   it). Rail cards show a modified count.
2. **Global search.** The existing filter box promotes to cross-mod search:
   with a query, the detail pane becomes a flat result list
   (`Mod › Group › Setting` breadcrumbs, click-to-jump-and-highlight). All
   data is already client-side; zero native work. The most "next-gen"
   daily-use feature in the set.
3. **Author-shipped presets** — zero native change:

   ```jsonc
   "presets": [
     { "label": "Performance", "description": "Lightweight HUD",
       "values": { "overlay.opacity": 0.6, "mode": "compact" } },
     { "label": "Cinematic", "values": { "overlay.opacity": 1.0, "mode": "full" } }
   ]
   ```

   Applied as a batch of ordinary `settings.set` (each natively validated —
   a malicious preset can't smuggle anything). Partial `values` fine.
4. **Hotkey conflict badges** (§9) — informational, never blocking.
5. **Restart-required aggregation** (§4.6).
6. **Session revert, not transactional apply.** Apply-immediately with live
   native reaction stays (MCM's OK/cancel dance existed because Scaleform
   couldn't do live feedback). The host keeps a session change-log (old→new
   per key); a "Session changes (4)" chip opens a recently-changed list with
   per-item revert and revert-all, replayed as `settings.set`. 90% of undo
   for 5% of the complexity, and it doubles as "recently changed".

**v2:** user profiles / import-export (§14 note: **clipboard is impossible**
— Ultralight has no clipboard handler by security design, so sharing = files
under `Documents\...\OSFUI\profiles\` + `settings.exportProfile /
importProfile / listProfiles` commands); controller/gamepad navigation
(widgets get `:focus-visible` states *now* so the retrofit is styling +
input routing, not rework); safe mode (launch with held key → schemas render,
values load as defaults, non-destructive until saved).

---

## 6. Tier 2 — the custom-panel escape hatch

The goal: a mod embeds interactive custom UI *inside its own settings page*
without shipping a whole separate view.

### 6.1 Mechanisms considered

- **Sanitized HTML fragment (no JS).** Safe, but it is §4.5 with more syntax
  — no interactivity, so it cannot deliver "interesting UI". Rejected as the
  escape hatch.
- **`<iframe>` in the host view.** Ultralight 1.4 iframe support is
  historically incomplete, and — decisive — everything is `file:///`
  same-origin, so an iframe provides **no actual isolation**: framed script
  can walk `parent.document`. All of the cost, none of the benefit. Rejected.
- **Separate Ultralight view composited into a slot.** True JS isolation, but
  it fights the architecture: views are full-screen surfaces composited by
  zorder with exclusive focus and a single-menu policy. A "slot" means
  rect-sync messages, focus juggling across the slot boundary, an extra
  full-screen CPU surface per panel, and per-frame scroll tracking — the
  worst case for the dirty-rect optimizer. A season of native work to poorly
  imitate one `<div>`. Rejected for now; revisit only if a real abuse
  incident makes isolation mandatory.
- **JS module loaded into the host view — chosen.** The sandbox already
  permits the settings view to load sibling-view assets
  (`security-model.md` rule 3 — the same mechanism as the shared
  `../shared/osfui.css` include).

The honest trust statement, to be repeated verbatim in authoring docs: **a
panel runs in the settings view's realm; it is trusted like any installed
mod's view.** This does not lower the native security boundary — the bridge
command whitelist and `SettingsStore` validation are the security model, and
a mod shipping its *own* view already runs JS with identical bridge power.
What a hostile/buggy panel *can* do is break the settings UI, spoof other
mods' rows, or spam repaints — availability/spoofing risks against other
mods, not native escalation. Containment (below) plus a user-facing kill
switch is the mitigation; an in-realm `MutationObserver` fence is theater and
is deliberately not built. Additionally, native per-view scoping of
`settings.set` ships in the same release as panels (decided, §14.3): a mod
view can only write its own mod ids (host settings view exempt), so even the
"identical bridge power" baseline tightens when this tier arrives.

### 6.2 Contract

Declaration lives in the schema, so the tier stays data-driven:

```jsonc
{ "id": "mymod", "title": "My Mod",
  "panel": { "view": "mymod", "entry": "mcm-panel.js",
             "height": 320, "placement": "top" },
  "groups": [ ... ] }
```

The entry file lives in the mod's `views/<id>/` folder (manifest-only,
non-surface folders are fine). The host loads it **lazily, only when the
mod's card is first selected**, one panel alive at a time (unmount on card
switch — bounds CPU cost and blast radius). The script registers rather than
self-executing into the DOM:

```js
// views/mymod/mcm-panel.js
osfmcm.register("mymod", {
  mount(root, ctx) { /* build DOM under `root` only */ },
  unmount()        { /* cleanup */ }
});
```

**Scoped context `ctx`** — panels never touch `window.osfui`; the host hands
them a relay pinned to the registering mod id:

- `ctx.get(key)` / `ctx.set(key, value)` — relayed to `settings.set` with
  `mod` pinned; ack surfaced as a promise.
- `ctx.onChange(cb)` — this mod's `settings.changed` deltas only.
- `ctx.command(name, fields)` — relayed as `ui.command` **only if `name`
  starts `"<modId>."`** (same rule as action buttons); `<modid>.*` replies
  routed to `ctx.onMessage`.
- `ctx.tokens` — read-only design-token snapshot (accent, spacing) so panels
  match the chassis without parsing CSS.

**Containment:** `mount()` wrapped in try/catch — a throwing panel is
replaced by an inline error card ("Panel failed — schema settings still
work") and not retried that session; slot gets `contain: layout paint` and
the schema's height cap; framework setting `osfui.allowPanels` (bool, default
true) is the user-facing kill switch.

**Why a registration object, not custom elements:** identical power, but the
host gets an explicit lifecycle (lazy mount/unmount, error boundary,
one-at-a-time) that `customElements.define` doesn't give, without betting on
Ultralight 1.4's custom-elements maturity.

---

## 7. Tier 3 — full custom view launch

```jsonc
"launch": { "view": "mymod-dashboard", "label": "Open Dashboard" }
```

The host renders a prominent launch button on the card/detail head; clicking
sends the existing `menu.open {view}`. The single-menu policy closes the
settings menu — correct behavior; the mod's view offers "Back to settings"
via `menu.open {view:"settings"}`. Native work: effectively zero. The point
is discoverability: the top tier launches from the same card as the bottom
tier — the whole ladder is visible in one place.

---

## 8. Registration & consumption architecture

### 8.1 Tier 0 hardening — drop-in JSON (exists; keep as flagship)

All inside `SettingsStore::LoadAll`:

- **`id` == filename stem, enforced** (warn-and-override on mismatch —
  implemented in `SettingsStore::AddSchema`, which also rejects ids outside
  `[A-Za-z0-9._-]`, with `..`/leading `.`, or equal to a reserved framework
  namespace). Enforcement makes MO2's per-file VFS priority *the*
  collision-resolution mechanism: two mods shipping `settings/coolmod.json`
  resolve like any other file conflict, and OSF UI never sees both. Residual
  duplicate ids: first wins, loud `REX::WARN`, conflict banner in the
  settings view.
- **Deterministic load order:** collect, sort by filename, then load
  (`directory_iterator` order is unspecified). UI sort stays title-based.
- **Sparse persistence:** persist only values ≠ schema default. Today
  `Persist` writes every value once any changes (`SettingsStore.cpp:288-310`),
  freezing untouched defaults forever; sparse files let upstream default
  changes reach users who never touched the knob, and make reset = key
  removal. (Migration interplay: §11.)
- **Debounced persistence:** a slider drag fires a `settings.set` per step —
  today each one is an atomic tmp+rename write. Keep notification immediate
  (live reaction is the point) but coalesce disk writes per mod (~500 ms
  write-behind, flushed on menu close and shutdown).
- Schema `"version"` field (§11) and `$KEY` l10n convention (§4.8) land here.

### 8.2 C ABI 1.2 — runtime registration + consumption for sibling DLLs

Appended vmethods on `IOSFUIBridge` (MINOR bump per the existing append-only
contract, `OSFUI_API.h:28-31`). JSON-string based, not struct-based — the
schema is already a JSON document with a formal JSON-Schema and a native
validator; a struct ABI would triple the surface for zero gain and violate
the API's own "only primitives and UTF-8 JSON text cross the boundary" rule.

```cpp
// --- settings registration. Thread-safe; merge lands on the next main tick. ---
// The SAME JSON document that would live in settings/<id>.json. Parse/shape
// errors report synchronously (false). Persisted values overlay from the same
// per-mod values file as the drop-in tier, so a mod can migrate tiers without
// losing user settings. Same id as a drop-in file: the DLL wins, with a warning.
virtual bool RegisterSettingsSchema(const char* a_schemaJson) = 0;
virtual void UnregisterSettingsSchema(const char* a_modId) = 0;

// --- change subscription. Fired on the GAME MAIN thread for every committed
// value of the subscribed mod — and REPLAYED once per current value on
// subscribe (and again if the mod registers later), so no separate initial
// read is needed. Per-mod, not per-key: one subscription, switch on a_key.
using SettingChangedFn = void (*)(const char* a_modId, const char* a_key,
                                  const char* a_valueJson, void* a_user) noexcept;
virtual std::uint32_t SubscribeSettings(const char* a_modId,
                                        SettingChangedFn a_fn, void* a_user) = 0;
virtual void          UnsubscribeSettings(std::uint32_t a_token) = 0;

// --- typed getters. Synchronous, callable from ANY thread — they read a
// mutex-guarded value mirror maintained on the main thread, never the store.
// false / 0 on unknown mod/key or type mismatch.
virtual bool GetSettingBool (const char* a_modId, const char* a_key, bool* a_out) = 0;
virtual bool GetSettingInt  (const char* a_modId, const char* a_key, std::int64_t* a_out) = 0;
virtual bool GetSettingFloat(const char* a_modId, const char* a_key, double* a_out) = 0;
// Covers string, enum (the option string), and key (the key NAME, e.g. "F10").
// Returns required length incl. NUL; copies min(bufLen).
virtual std::uint32_t GetSettingString(const char* a_modId, const char* a_key,
                                       char* a_buf, std::uint32_t a_bufLen) = 0;
```

Callback delivery rides the existing `PumpMainThread` queue, so "all engine
interaction on the main thread" holds by construction. Getters read a
`BridgeApi`-owned mirror map (updated as a store subscriber on the main
thread) — lock-cheap and any-thread, which is exactly what Papyrus needs too
(§8.4). Subscribing to a not-yet-registered mod id is legal; the replay fires
when it registers (load-order insurance). Registrations live in
`SettingsStore` / `BridgeApi`, both independent of `MessageBridge` lifetime,
so they survive bridge re-creation for free — same pattern as command
registrations today.

### 8.3 SettingsStore generalization (the enabler)

- Single `ChangeListener` slot → small **multicast list** (Runtime's core
  listener becomes subscriber #0; the `SettingsModule` wiring at
  `Runtime.cpp:861-864` is unchanged in spirit).
- `RegisterSchema(json schema, Source source)` — incremental add/replace
  after `LoadAll`, overlays persisted values, bumps a **registry generation**
  counter, fires per-mod value replay.
- `RemoveMod(id)`, `GetValue(modId, key)`, `GetSettingType(modId, key)`,
  `NotifyMod(modId)`.

Everything else in this document is a projection over these.

### 8.4 Papyrus surface — consumption only (later milestone, committed)

**Feasibility confirmed in-tree:** `IVirtualMachine::BindNativeMethod` with
automatic type marshaling exists in the pinned CommonLibSF
(`RE/B/BSScriptUtil.h`). Bind when the VM exists (from the existing
`OnSFSEMessage` handler, `kPostDataLoad`, guarded on `RE::GameVM`).

Shipped `OSFUI.psc`:

```papyrus
ScriptName OSFUI Hidden

bool   Function GetBool  (string modId, string key, bool abDefault = false)  Global Native
int    Function GetInt   (string modId, string key, int aiDefault = 0)       Global Native
float  Function GetFloat (string modId, string key, float afDefault = 0.0)   Global Native
string Function GetString(string modId, string key, string asDefault = "")   Global Native ; string/enum/key
int    Function GetVersion() Global Native   ; 0 => OSF UI absent (feature-detect)
Function SetBool  (string modId, string key, bool value)   Global Native     ; fire-and-forget
Function SetInt   (string modId, string key, int value)    Global Native
Function SetFloat (string modId, string key, float value)  Global Native
Function SetString(string modId, string key, string value) Global Native
```

**The threading rule that makes this safe:** Papyrus natives run on VM
tasklet threads — they read the same any-thread mirror as the C ABI getters,
never `SettingsStore` directly. Setters enqueue onto the main-thread pump and
return immediately; validation and persistence happen through the normal
`SettingsStore::Set` path — **Papyrus gets no bypass**.

Deferred: `RegisterForSettingChange` push events into script objects (getters
cover the dominant MCM pattern — read config in `OnInit`/on demand; correct
event delivery into Papyrus is the fiddly part). Sketch for later:
`Function RegisterForSettingChanges(ScriptObject receiver, string modId)` →
`Event OSFUI_OnSettingChanged(string modId, string key)`.

### 8.5 Web/JS consumers

- **`settings.changed` push:** `SettingsModule` subscribes to the store and
  pushes `{ type:"settings.changed", payload:{ mod, key, value } }` to every
  view that has called `settings.get` — subscribe-on-read, copying the
  `views.get`/`_viewsSubscribers` pattern (`Runtime.cpp:950-955`). A mod's
  own HUD view reacts live to its settings with zero polling and zero native
  code.
- **Registry refresh:** on registry-generation change (runtime
  registration/unregistration while the menu is open), re-broadcast
  `settings.data` to subscribers — the settings view already fully re-renders
  on that message, so late registration Just Works with no view changes.
- Both documented in `sdk/osfui.d.ts` + `docs/authoring-views.md` (which can
  then delete its "reacting natively requires a core change" caveat).

---

## 9. Keybinding generalization + HotkeyService

- **Capture allowlist → schema-driven.** Replace the hardcoded
  `mod != "osfui" || key != "toggleKey"` check (`Runtime.cpp:963`) with
  `GetSettingType(mod, key) == "key"`; store `_captureMod` alongside
  `_captureKey` and fix the hardcoded `{"mod","osfui"}` reply in
  `DrainKeyCapture` (`Runtime.cpp:884-885`). The web side is already fully
  generic — this is native-only work.
- **Conflict detection: informational, never blocking.** On any `key`-type
  commit and at load, build `vk → [(mod, key)]` via `ResolveKeyName`; embed
  `conflicts: [{mod, key, title}]` per key-setting into `DataJson()`; the
  view renders a warning badge on both sides ("Also bound by: Cool Mod") and
  live-warns during capture. Rejecting binds would punish the second mod's
  user for the first mod's default — don't.
- **Central `HotkeyService` (decided; new `src/runtime/` component).**
  Rationale: (a) OSF UI already owns the input hook *and the policy context*
  — a mod's own raw hook can't know a press happened while the user was
  typing in an OSF UI text field or mid-rebind, so per-mod hooks
  systematically double-fire; (b) Papyrus and web-only mods have no input
  hook at all — this service is the only way their `key` settings ever *do*
  anything; (c) one rebuild point when a binding changes.
  - Keeps `vk → subscribers`, rebuilt from the store on any `key`-type
    change; fed from the existing `OnHostKey` path (which already knows
    capture state); suppressed while the overlay captures input or a rebind
    is armed.
  - C ABI (same 1.2 bump):
    `using HotkeyFn = void (*)(const char* modId, const char* key, void* user) noexcept;`
    + `SubscribeHotkey(modId, key, HotkeyFn, user) → token` /
    `UnsubscribeHotkey(token)`. Main-thread delivery via the pump.
  - Web: `{ type:"ui.hotkey", payload:{ mod, key } }` pushed to the owning
    mod's subscribed views — "toggle my HUD" with zero native code.
  - Papyrus hotkey events: deferred with the Papyrus event system.
  - Every `key`-typed setting participates; subscription is the opt-in — no
    extra schema flag.
- **Vanilla hotkeys (v1, no engine RE — built 2026-07-14).** The game's own
  bindings join the conflict grouping as pseudo-entries under the reserved
  mod id `@game` (title "Starfield (Quicksave)"); badges + capture live-warn
  work unchanged, and the HotkeyService registry is untouched. Source of
  truth: Starfield ships NO controlmap data file (verified: no
  `controlmap` entry in any GNRL ba2 — the defaults live in the executable;
  CommonLibSF has only RTTI/REL ids for the live `ControlMap` singleton,
  867482/867486/867487, no layout). So v1 is a curated, shipped
  `vanillakeys.json` (editable; gameplay-context keyboard defaults only),
  overlaid at load by the same controlmap text files the engine honors —
  `Data/Interface/Controls/PC/ControlMap.txt` (mod-provided override) then
  `Documents/My Games/Starfield/ControlMap_Custom.txt` (in-game remaps);
  DIK scan codes translate via `MapVirtualKey(VSC_TO_VK_EX)`
  (`Platform::DirectInputScanToVk`). Config kill-switch:
  `vanillaKeyConflicts` (default true). **v2 (needs an RE pass):** read the
  live ControlMap singleton for in-session remap fidelity, per-context
  accuracy, and gamepad — swaps in behind `VanillaKeys::Bindings()`.

---

## 10. Ordering & lifecycle

- **Timeline (verified):** drop-in `LoadAll` at SFSE `kLoad` → sibling DLLs
  `RequestBridge` + `RegisterSettingsSchema` at `kPostLoad`+ → queued, merged
  on first main ticks → UI readiness much later. The store is
  bridge-independent, so every ordering is safe; the only requirement is
  incremental `RegisterSchema` (§8.3).
- **Fire-on-subscribe replay** (at subscribe time and at late registration)
  replaces any need for consumers to sequence against startup `NotifyAll` —
  a DLL that subscribes before its own schema merges still gets its replay
  when the merge commits.
- **Late registration with the menu open:** generation bump →
  `settings.data` re-broadcast → view re-renders. No special-casing.
- **Uninstalled mods:** schema absent → the mod vanishes from UI and
  registry; its values file in Documents is **kept indefinitely**. MO2
  profile switching makes "uninstalled" indistinguishable from "temporarily
  disabled", and the files are tiny. Never auto-delete; an optional
  "orphaned settings" housekeeping panel is a later nicety.

---

## 11. Versioning & migration

> **Built 2026-07-14.** All three below shipped and host-tested
> (`settings_store_tests` §11 blocks). Programmatic hooks remain deferred.

- **`"version": <uint>`** on the schema (default 0). Values files gain a
  reserved `$schemaVersion` meta key — safe because the loader only reads
  keys the schema declares (`ForEachSetting` walks `groups[].settings[]`), so
  `$`-prefixed keys never enter `mod.values` and are invisible to old builds.
  Log on upgrade/downgrade. **A v0 mod is never stamped** — bumping to 1+ is
  opt-in, so no existing values file churns just from this landing. The stamp
  round-trips through `SparseValues` so a current-version file loads clean
  (no perpetual re-dirty).
- **Renamed keys: per-setting `"aliases": ["oldKey", ...]`** rather than a
  version-indexed migration table. On load, if the saved file lacks the
  current key (or its value no longer validates), adopt the first alias whose
  stored value re-validates; the next write lands under the current key and
  the old alias key drops (SparseValues only emits declared keys). The
  current key always wins over an alias. Local, declarative, zero version
  arithmetic. Type changes fall through validation to the new default —
  already today's behavior.
- **Default-value changes:** solved by sparse persistence (§8.1); the
  prune-to-default-on-load path (`saved != SparseValues` → rewrite) already
  sheds values that now equal an updated default. The only loss is "user
  explicitly chose the old default", which is unknowable anyway (open
  question §14).
- **Programmatic migration hooks** (native callback with old version + old
  values): deferred indefinitely — ABI surface waiting for a use case.

---

## 12. Authoring DX

The target journey: *copy a folder, edit one JSON with autocomplete yelling
at you, alt-tab, press a key, see it.*

1. **Schema hot-reload — the highest-leverage DX feature.** ✅ Built
   2026-07-14. In `devMode`: `SettingsModule::PumpSchemaHotReload`
   mtime-polls `settings/*.json` on a ~1 s cadence from `Runtime::Tick`
   (mtime snapshot seeded at construction; recorded per attempt so a torn
   editor save retries on the final write without per-scan log spam).
   A changed/new file goes through `SettingsStore::ReloadDropInFile` —
   AddSchema's replace path with one relaxed precedence rule (drop-in may
   replace drop-in; a runtime registration still outranks the file both
   ways, including deletion). Values survive via the same flush-dirty +
   overlay-from-file path as runtime re-registration, so §11 aliases carry
   values across a live key rename too; the registry listener re-broadcasts
   `settings.data` and the open view repaints. A deleted file drops its
   (drop-in) mod; values files are kept (§10). Same pass added the dev-mode
   **view-reload keybind**: config `devReloadKey` (default F11, resolved
   only in devMode), consumed in `OnHostKey` like the toggle key, drained
   in Tick (`DriveDevReload`) — the crash-recovery `LoadView` + `Resize`
   pair on the top open menu.
2. **Browser dev harness.** The settings view's standalone fallback already
   proves the pattern; formalize it: `devtools/harness/` loads the *real*
   settings view assets plus a `MockBridge` that accepts a schema via
   drag-drop or `?schema=`, implements `settings.get/set/reset/captureKey`
   with localStorage persistence and clamp rules mirrored from
   `SettingsStore` (drift risk noted in its header), and logs exact bridge
   traffic. Authors iterate schemas *and Tier-2 panels* without launching
   Starfield.
3. **Editor autocomplete — mostly done.** Publish
   `settings-schema.schema.json` at a stable URL; put `"$schema": "<url>"`
   in every example and template; extend the schema **in the same PR** as
   each renderer addition (the existing lockstep discipline for
   schema/`.d.ts`/docs is a genuine differentiator — keep it).
4. **Templates.** `examples/` with three folders matching the tiers:
   `settings-only/` (annotated schema exercising every widget),
   `settings-plus-panel/`, `full-view/`. The quickstart is "copy
   `settings-only`, rename, done in 5 minutes."
5. **Validation:** v1 documents
   `npx ajv-cli validate -s settings-schema.schema.json -d mymod.json`;
   a bespoke `osfui lint` (duplicate keys, conditions referencing unknown
   keys, non-namespaced action commands, presets failing clamp) is v2 — the
   harness surfaces most of these interactively anyway.
6. **Docs restructure.** `authoring-views.md` is view-first; the MCM story
   needs a settings-first page ordered exactly as the ladder: "Add settings
   to your mod" quickstart → widget gallery (harness screenshots) →
   conditions cookbook → panel guide → launch guide.

Visual identity, briefly: don't redesign the chassis — systematize it.
Per-mod accent everywhere (hash-derived like the hub, or an optional
host-validated schema `"accent"`); state as signal lamps (modified dot =
accent glow, restart banner = `--signal-warn`, conflict = `--signal-stop`);
motion discipline for the CPU renderer (transitions only on small contained
elements, `opacity`/`transform` ≤150 ms, never pane-level layout — and this
rule goes in the panel-author docs with `contain: paint` guidance).

---

## 13. Milestones

> **Build status (2026-07-04).** The **client-side renderer slice is
> implemented and verified headless** — it needs no native changes and works
> in-game today against the existing `settings.data`/`settings.set`/
> `settings.reset` bridge. Shipped: schema power-ups (conditions, widget hints,
> number formatting, `widget:"color"`, `note`/`image`, `action` buttons,
> `requires` badges), platform UX (global search, modified-dots + per-setting
> reset, presets, session-changes revert, restart banner, auto section index),
> the forward-compat degrade rule (unknown type → read-only), the contract in
> lockstep (`settings-schema.schema.json` + `sdk/osfui.d.ts`, protocol 0.3), an
> annotated `examples/settings-only/` template, and a browser dev harness
> (`devtools/harness/`) with a validation-mirroring MockBridge. Verified by a
> jsdom suite driving the real shipped files (widgets, conditions, actions,
> presets, search, revert, injection-safety, and the mock clamp/persist path).
> **Build status (2026-07-12).** Two more M1 slices are **implemented**:
>
> - **`SettingsStore` generalization (§8.3)**: multicast change listeners,
>   incremental `RegisterSchema` with Source precedence (drop-in vs native,
>   per §14.1), `RemoveMod`, `GetValue`/`GetSettingType`, `NotifyMod` replay,
>   a registry-generation counter + registry-change listeners, and
>   deterministic (filename-sorted, first-wins) drop-in loading.
> - **Web change delivery (§8.5)**: `settings.get` now subscribes the calling
>   view; every committed value pushes as `settings.changed {mod,key,value}`
>   and registry shape changes re-broadcast `settings.data`. Bridge protocol
>   bumped to **0.3** (`Version.h` + `osfui.d.ts` + `authoring-views.md` in
>   lockstep).
>
> Verified by host-side suites (`tests/native/`, 242 checks across four
> suites) that compile the real store/module/bridge/api sources on the
> desktop toolchain against a REX stub — no Windows build required for this
> logic. In-game verification on the Windows build is still pending.
>
> **Also built since (2026-07-13):** the C ABI 1.2 settings surface — the
> any-thread value mirror + typed getters, `SubscribeSettings` with
> replay-on-subscribe, `RegisterSettingsSchema`/`Unregister` (§8.2;
> `src/api/SettingsMirror.*`, `src/api/SettingsSubscriptions.*`,
> `BridgeApi`, `sdk/OSFUI_API.h` bumped to 1.2) — and sparse + debounced
> write-behind persistence (§8.1).
>
> **Also built (2026-07-13):** the HotkeyService core (§9) — every key-typed
> setting dispatches through `src/runtime/HotkeyService.*` (registry rebuilt
> on rebind/registration, suppressed while capturing/rebinding), delivered
> over C ABI **1.4** (`SubscribeHotkey`, `src/api/HotkeySubscriptions.*`) and
> as the `ui.hotkey` web push (protocol **0.4**), with informational
> `conflicts` data embedded per key-setting in `settings.data`. Host-tested
> (six suites, 313 checks); in-game verification pending. The renderer-side
> conflict badges and the keybindings UI are not built yet.
>
> **Not yet built (needs the Windows/CommonLibSF build):**
> `color`/`flags` as native-validated types, and the Papyrus surface.
> `type:"color"` ships as `widget:"color"` on a `string` until the native
> validator lands.

Ordering of the M1/M2 UI-vs-plumbing split is suggested, not contractual.

- **M1 — "MCM ships":** store multicast + incremental `RegisterSchema` +
  getters; C ABI 1.2 (`RegisterSettingsSchema`, `SubscribeSettings`, typed
  getters, mirror cache); generalized key capture; `settings.changed` push +
  `settings.data` re-broadcast; drop-in hardening (id==stem, sorted load,
  sparse persistence, duplicate warning); v1 schema power-ups (conditions,
  widget hints, `color`, actions, note/image, `requires`) and v1 platform UX
  (search, modified dots + per-row reset, presets, session revert) in the
  settings view.
- **M2:** HotkeyService (C ABI + `ui.hotkey`), conflict badges, Tier-2 panel
  API, Tier-3 launch button, **l10n loading + translation-mod support**
  (§4.8 — pulled forward of Papyrus because translation mods are an
  ecosystem-adoption lever), schema hot-reload + dev reload key, browser
  harness, templates + settings-first docs.
- **M3:** Papyrus natives (getters + validated fire-and-forget setters,
  `GetVersion`), `flags` + `pages[]`, user profiles (file-based),
  orphaned-values housekeeping. Stretch: Papyrus change/hotkey events,
  controller nav, safe mode, `osfui lint`.

**File-level touch points** (for implementation planning):

| Area | Files |
|---|---|
| Store core (incremental reg, multicast, getters, aliases, sparse persist, l10n, generation) | `src/runtime/SettingsStore.{h,cpp}` |
| Web push + registry refresh | `src/runtime/SettingsModule.{h,cpp}` |
| C ABI 1.2 (schema reg, subscriptions, getters, mirror, hotkey sub) | `sdk/OSFUI_API.h`, `src/api/BridgeApi.{h,cpp}`, `src/api/Exports.cpp` |
| Capture generalization, conflict data, hotkey wiring | `src/runtime/Runtime.cpp` (:869-893, :956-971, :1010-1049), new `src/runtime/HotkeyService.{h,cpp}` |
| Key mapping reuse | `src/input/InputRouter.{h,cpp}` |
| Papyrus (M3) | new `src/papyrus/PapyrusSettings.{h,cpp}`, hook in `src/core/Plugin.cpp` (`OnSFSEMessage`), shipped `OSFUI.psc` |
| Renderer (most v1 UX) | `data/OSFUI/views/settings/main.js`, `data/OSFUI/views/shared/osfui.css` |
| Contracts/docs | `docs/schema/settings-schema.schema.json`, `sdk/osfui.d.ts`, `docs/native-plugin-api.md`, `docs/authoring-views.md` |

---

## 14. Decisions & open questions

**Resolved (maintainer, 2026-07-03):**

- Tier-2 panels: **yes** — shared-realm with containment + `osfui.allowPanels`
  kill switch (§6).
- Papyrus: **yes, getters + setters** (validated, fire-and-forget), as a
  later milestone (§8.4).
- Hotkeys: **central HotkeyService**, not bring-your-own-hook (§9).
- This document precedes implementation; no code changes ship with it.

**Resolved (maintainer, 2026-07-04):**

1. **Id precedence:** same id from both a DLL and a drop-in file — the DLL
   wins, with a warning naming both sources (a mod upgrading tiers must not
   require users to hand-delete the stale JSON).
2. **Prune-to-default:** on first load under sparse persistence, prune
   persisted values equal to the current schema default (one-time cleanup).
3. **Per-view scoping of `settings.set`:** yes — native enforcement (a view
   may only write its own mod ids; host settings view exempt), landing in
   the same release as Tier-2 panels. Small `MessageBridge`/`Runtime` change
   that closes the latent any-view-writes-any-mod gap.
4. **Hotkey delivery while the overlay is open: always suppressed.** No
   per-subscription opt-out for now; a flag can be appended to the ABI later
   if a real need appears.
5. **Language source:** `osfui.language` setting, `"auto"` default reading
   the game's `sLanguage` INI (§4.8).
6. **Protocol versioning:** this wave ships as bridge protocol **0.3**; the
   1.0 freeze happens after M2, once the panel API and l10n shapes have
   survived contact with real authors.
7. **Panel asset location:** reuse `views/<id>/` (existing sibling-asset
   sandbox rule; manifest-only folders fine) — no sandbox change.
8. **`textarea` cap:** per-setting `maxLength` may raise the limit up to a
   new hard ceiling of 4096 (default stays 256 when unspecified).
9. **Per-character scope: reserve only.** `"scope": "global" | "character"`
   is claimed in the schema spec now; non-`global` values are rejected with
   a clear "not yet supported" warning. Per-character storage (values
   swapped on SFSE `kPostLoadGame`, scope badge in UI, stable
   character-identity spike) is implemented only when a real mod asks.

**Open:**

1. **Non-Latin font coverage** (§4.8): verify Ultralight's fallback stack
   for CJK/Cyrillic; bundle a Noto subset vs document a system-font
   requirement. Needs a spike, not an opinion — must be answered before
   localization is advertised (target: before M2 ships).
2. **Corrupt/hand-edited values files:** today a parse failure silently
   falls back to defaults. With sparse persistence that's indistinguishable
   from "user reset everything". Standing recommendation (unchallenged): on
   parse failure, rename the bad file to `<id>.json.bad` and warn in the log
   + a settings-view banner, so user edits are never silently discarded.
