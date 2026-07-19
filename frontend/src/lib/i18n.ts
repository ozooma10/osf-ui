// i18n.ts — the per-view translator factory.
//
// Both legacy views define the same three-line helper over the frozen bridge
// helper's `osfui.t`, differing only in the namespace they hard-code:
//
//   settings/main.legacy.js:98
//     function tr(address, english, vars) {
//       return typeof osfui.t === "function"
//         ? osfui.t("chrome.settings." + address, english, vars) : english;
//     }
//   keybinds/main.legacy.js:42 — identical, with "chrome.keybinds." instead.
//
// This module is that helper, with the namespace injected instead of baked in.
// It stays pure (no DOM, no globals): it takes the `t` capability off a Bridge,
// so tests can drive it with `nullBridge` or a stub.
//
// ---------------------------------------------------------------------------
// THE FIX: absolute vs. namespaced addresses
// ---------------------------------------------------------------------------
// The legacy helpers are unconditional string concatenations, so neither view
// can ever address a SHARED catalog entry. "chrome.common.loading" is a real,
// shipped address — both index.html files reach it, but only through the
// declarative `data-i18n` attribute path (settings/index.html:71,
// keybinds/index.html:72), which calls `osfui.t` directly and therefore skips
// the prefix. Any script-side attempt would ask the catalog for
// "chrome.settings.chrome.common.loading" and silently fall through to the
// authored English forever.
//
// So: an address containing a "." is treated as ALREADY ABSOLUTE and passed
// through untouched; a bare token gets the view's prefix. This is safe against
// every address the two views actually use — all of them are bare camelCase
// tokens ("writeRejected", "presetAppliedOne", "gameOwner", ...), so no
// existing lookup changes address. It is a strict widening: previously
// unreachable addresses become reachable, and nothing that resolved before
// resolves differently.

import type { Bridge } from '@lib/bridge';

/** Interpolation variables. Mirrors the frozen helper's accepted value types. */
export type TranslationVars = Record<string, string | number>;

/**
 * The `t` capability, narrowed to what this module needs. Structural typing
 * means any `{ t }` object satisfies it, so tests need no Bridge stub.
 */
export type TranslatorHost = Pick<Bridge, 't'>;

export interface Translator {
  /**
   * Look up `address` (bare token -> prefixed; anything containing a "." ->
   * absolute), falling back to the authored `english`.
   */
  (address: string, english: string, vars?: TranslationVars): string;

  /**
   * The two-form plural selection both views hand-roll, e.g.
   * settings/main.legacy.js:793 and :981, keybinds/main.legacy.js:322.
   *
   * This is NOT ICU: the form is chosen by appending "One"/"Other" to the
   * address, and the catalog carries two separate entries. Selection is a
   * strict `count === 1`, so 0 and negatives take the Other form — matching
   * English but NOT languages with a zero/few/many category. Preserved as-is;
   * changing it would silently re-address every existing catalog entry.
   *
   * `{ count }` is merged into `vars`, and an explicit `vars.count` still wins
   * so a caller can pre-format it.
   *
   * SMALL DEVIATION, stated plainly: the legacy call sites are inconsistent
   * about passing `count`. Most do for both forms (`{ count: menus }`,
   * settings/main.legacy.js:793-794), but the One branch at :877 passes NO vars
   * at all — its authored English ("1 settings file failed to load") hardcodes
   * the 1. So under legacy, a translator who wrote "{count} Datei" into the
   * *One* entry of that address would see the braces render literally; here it
   * interpolates. Nothing in-tree changes (no shipped One-form string contains
   * a placeholder), and the merge is what makes `plural` usable uniformly — but
   * it is a widening, not a transcription.
   */
  plural(
    base: string,
    count: number,
    one: string,
    other: string,
    vars?: TranslationVars,
  ): string;
}

/**
 * True when `address` is a fully-qualified catalog address rather than a token
 * local to one view's namespace. See the fix note at the top of this file.
 */
export function isAbsoluteAddress(address: string): boolean {
  return address.includes('.');
}

/**
 * Build a view-scoped translator.
 *
 * @param bridge anything carrying a `t` — the real {@link Bridge},
 *   `nullBridge`, or a bare `{ t }` stub.
 * @param prefix the view namespace, with or without its trailing dot:
 *   "chrome.settings" and "chrome.settings." behave identically. An empty
 *   prefix makes every address absolute.
 */
export function makeTranslator(bridge: TranslatorHost, prefix: string): Translator {
  // Normalise once so call sites can write either spelling. The legacy sources
  // embed the trailing dot in the literal; the parameter form invites dropping
  // it, and a missing dot would corrupt every address.
  const ns = prefix && !prefix.endsWith('.') ? `${prefix}.` : prefix;

  const tr = ((address: string, english: string, vars?: TranslationVars): string =>
    bridge.t(isAbsoluteAddress(address) ? address : ns + address, english, vars)) as Translator;

  tr.plural = (base, count, one, other, vars) =>
    tr(base + (count === 1 ? 'One' : 'Other'), count === 1 ? one : other, {
      count,
      ...vars,
    });

  return tr;
}
