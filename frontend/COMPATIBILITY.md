# Retained compatibility boundaries

The authored 2.0 helper, stylesheet, and private pad navigation are hand-written
compatibility boundaries rather than TypeScript output. The stylesheet and pad
navigation are copied verbatim. During 2.0.x, the shipped helper is composed
deterministically from the byte-unchanged 2.0 core plus the guarded
`src/compat/v1/osfui-v1.js` façade; `scripts/verify-output.mjs` asserts that exact
composition on every build.

This file records *why* each boundary exists and *what has to be true* before it
is dissolved. Do not convert one of these on a whim — each exit criterion below
exists because the failure mode is invisible until the game is running.

---

## 1. `src/shared-kit/osfui.js` → `build/frontend/views/shared/osfui.js`

**Status: frozen 2.0 core. Default position is to never transform this file.**

This is the published bridge helper, protocol 2.0. Its own
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

The temporary append step is not a rewrite of this file and activates only on
navigation carrying `osfui-api=1`. Remove that append in 2.1.0 using
[`docs/compat-v1-removal.md`](../docs/compat-v1-removal.md).

**Exit criterion:** do not transform it. If it ever must change, change `src/shared-kit/osfui.js`
directly as hand-written JavaScript, bump the bridge protocol version in
`src/core/Version.h` and `sdk/osfui.d.ts` together, and treat it as a public API
release. Compiling it is a separate, deliberate decision requiring a byte-diff
gate against the previous output.

## 2. `src/shared-kit/osfui.css` → `build/frontend/views/shared/osfui.css`

**Status: frozen, same contract as above.**

The design-token kit. Its contract is explicit: the `osf-*` / `--osf-*` prefixes
are reserved, opt-in is all-or-nothing, and it sets global element base styles
that third-party views inherit. Passing it through a CSS pipeline would risk
reordering or minifying declarations that other people's stylesheets cascade
against.

**Exit criterion:** as above. Note that `--osf-*` tokens are still consumed from
TypeScript — but generated *from* this file, never the reverse.

## 3. `src/legacy/padnav.js` → `build/frontend/views/osfui/padnav.js`

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
| `e.keyCode`, **not** `e.key` | retained behavior of the existing navigation helper |

Converting it means rewriting geometry-dependent spatial navigation whose in-game
controller verification was still pending. jsdom cannot validate it: every
element has zero size there, so `getBoundingClientRect`-driven logic gives false
confidence in both directions.

The Preact views therefore **reproduce padnav's DOM contracts**, and
`test/dom-contracts.test.tsx` asserts they still do.

**Exit criterion — both must hold:**

1. A manual controller pass over both views passes in game: rail → detail → each
   widget type → undo modal → Keybindings view → binding list.
2. The conversion is done in a change that touches **nothing else**, so a
   navigation regression cannot be confused with a rendering one.

Until then `padnav.js` ships as-is and the views adapt to it, not the reverse.

## What is NOT a boundary

`main.js` and `style.css` for **every** view are fully generated from
`src/views/osfui/<view>/`. All three built-in views (`settings`, `keybinds`,
`handoff`) are Preact/TypeScript bundles; the phased port is
complete and no legacy bundle path remains.
