// Per-view translator factory over the frozen bridge helper `osfui.t`, with the
// view namespace injected rather than baked in. Pure: no DOM, no globals — it
// takes the `t` capability off a Bridge, so tests can pass `nullBridge` or a stub.
//
// Addressing: an address containing a "." is already absolute and is passed
// through untouched; a bare token gets the view's prefix. Without this, shared
// catalog entries are unreachable from script — "chrome.common.loading" would be
// looked up as "chrome.settings.chrome.common.loading" and fall through to the
// authored English forever. Every address the views use is a bare camelCase token
// ("writeRejected", "presetAppliedOne", "gameOwner", ...), so no existing lookup
// changes address.

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
   * Two-form plural selection. Not ICU: the form is chosen by appending
   * "One"/"Other" to the address and the catalog carries two separate entries.
   * Selection is a strict `count === 1`, so 0 and negatives take the Other form —
   * right for English, wrong for languages with zero/few/many. Left as-is;
   * changing it would silently re-address every existing catalog entry.
   *
   * `{ count }` is merged into `vars`; an explicit `vars.count` wins so a caller
   * can pre-format it.
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
 * local to one view's namespace.
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
  // Normalise once: a missing trailing dot would corrupt every address.
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
