# Retained compatibility boundaries

Four artifacts are **copied verbatim** by `scripts/build.mjs` rather than being
compiled from TypeScript. Each is a deliberate boundary, not unfinished work.
`scripts/verify-output.mjs` asserts each one is byte-identical to its source on
every build, so a boundary cannot rot silently.

This file records *why* each boundary exists and *what has to be true* before it
is dissolved. Do not convert one of these on a whim — each exit criterion below
exists because the failure mode is invisible until the game is running.

---

## 1. `src/shared-kit/osfui.js` → `data/OSFUI/views/shared/osfui.js`

**Status: frozen. Default position is to never generate this.**

This is the published bridge helper, protocol 1.0, api-freeze item 5. Its own
header calls it "part of the frozen contract". Third-party mods load it by
exact path:

```html
<script src="../../shared/osfui.js"></script>
```

It is loaded by a classic `<script>` tag *before* the view's own bundle, and it
**owns `window.osfui.onMessage`**. Regenerating it through a bundler risks
byte-level behaviour change — different `this` binding, different timer
semantics under minification, a changed property enumeration order — against an
unknown population of third-party consumers, for zero user-visible gain.

**Exit criterion:** do not. If it ever must change, change `src/shared-kit/osfui.js`
directly as hand-written JavaScript, bump the bridge protocol version in
`src/core/Version.h` and `sdk/osfui.d.ts` together, and treat it as a public API
release. Compiling it is a separate, deliberate decision requiring a byte-diff
gate against the previous output.

## 2. `src/shared-kit/osfui.css` → `data/OSFUI/views/shared/osfui.css`

**Status: frozen, same contract as above.**

The design-token kit. Its contract is explicit: the `osf-*` / `--osf-*` prefixes
are reserved, opt-in is all-or-nothing, and it sets global element base styles
that third-party views inherit. Passing it through a CSS pipeline would risk
reordering or minifying declarations that other people's stylesheets cascade
against.

**Exit criterion:** as above. Note that `--osf-*` tokens are still consumed from
TypeScript — but generated *from* this file, never the reverse.

## 3. `src/legacy/padnav.js` → `data/OSFUI/views/osfui/padnav.js`

**Status: private, unfrozen, but retained — this is the one with a real path forward.**

Spatial gamepad/focus navigation. Its header states it is *"deliberately PRIVATE
to the osfui views… not part of the shared kit… this must be able to change
shape freely."* So unlike the shared kit, it is *allowed* to change — it simply
should not change **yet**.

It navigates by reading concrete DOM geometry and conventions:

| Contract | Meaning |
|---|---|
| `.row` ancestors | navigation bands |
| `.listening` | suspends all navigation (a key capture is armed) |
| `[data-nav-modal]` | focus trap boundary |
| zero-size or `opacity: 0` | treated as invisible, skipped |
| `e.keyCode`, **not** `e.key` | deliberate — Ultralight emits legacy `"Up"` / `"U+00XX"` spellings |

Converting it means rewriting geometry-dependent spatial navigation whose in-game
controller verification was still pending. jsdom cannot validate it: every
element has zero size there, so `getBoundingClientRect`-driven logic gives false
confidence in both directions.

The Preact views therefore **reproduce padnav's DOM contracts**, and
`test/dom-contracts.test.tsx` asserts they still do.

**Exit criterion — all three must hold:**

1. A manual controller pass over both views passes in game: rail → detail → each
   widget type → undo modal → keybinds board → bind list.
2. That pass is repeated on **both** renderer backends (`ultralight` and
   `webview2`), because their key-event spellings differ.
3. The conversion is done in a change that touches **nothing else**, so a
   navigation regression cannot be confused with a rendering one.

Until then `padnav.js` ships as-is and the views adapt to it, not the reverse.

## 4. `src/views/osfui/*/manifest.json`

**Status: hand-authored configuration, not code.**

Copied unmodified. Qualified ids (`osfui/settings`, `osfui/keybinds`) are
unchanged by this migration and must stay that way — `config.json`, the Papyrus
API, and any third-party `menu.open` call reference them.

One inconsistency is preserved on purpose: `keybinds/manifest.json` carries a
`$schema` key pointing five levels up, and `settings/manifest.json` carries none.
The path is relative to the *source* location and does not resolve from the
deployed directory. That is pre-existing, harmless (native `ViewManifest`
ignores unknown keys), and normalising it would change shipped bytes for an
editor-only convenience.

---

## What is NOT a boundary

`main.js` and `style.css` for both views are **fully generated** from
`src/views/osfui/<view>/`. The pre-migration hand-written sources are retained
beside them as `main.legacy.js` purely as a behavioural reference during review;
they are excluded from `tsconfig.json` and are not in any build graph. Once both
views have had an in-game verification pass, delete them — they are the single
biggest source of "which file is real?" confusion for a newcomer.
