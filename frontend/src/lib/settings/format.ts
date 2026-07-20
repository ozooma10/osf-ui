// format.ts — display formatting for numeric settings, enum options and the
// `requires` badge (main.legacy.js:249-267, 540-542).
//
// The stored value is always the raw number; everything here is presentation.

import type { Setting } from '@sdk';

/**
 * Hard bounds on `format.decimals`.
 *
 * `Number.prototype.toFixed` throws a RangeError outside its supported range.
 * An uncaught throw inside the row builder aborts `renderDetail` and blanks the
 * WHOLE DETAIL PANE — a schema typo (`decimals: 21`) would take out the entire
 * mod page. See the comment at main.legacy.js:254-256. Clamping is therefore a
 * crash guard, not cosmetics; 20 remains the documented schema limit.
 */
export const MIN_DECIMALS = 0;
export const MAX_DECIMALS = 20;

/** Just the setting fields `formatNumber` reads. */
export type NumberFormatSource = Pick<Setting, 'type' | 'format'>;

/**
 * Render a numeric setting's value for display.
 *
 * `value` is `unknown` on purpose: the legacy slider path passes the DOM
 * input's `value`, a STRING, straight in (main.legacy.js:393), relying on
 * `Number(v) * scale` to coerce. Preserved so the ported slider can do the same.
 */
export function formatNumber(setting: NumberFormatSource, value: unknown): string {
  const f = setting.format || {};
  // `typeof === "number"` not `??`: a non-numeric `scale` in untrusted schema
  // JSON falls back to 1 instead of producing NaN.
  const scale = typeof f.scale === 'number' ? f.scale : 1;
  const n = Number(value) * scale;
  let s: string;
  if (typeof f.decimals === 'number') {
    // `f.decimals | 0` truncates toward zero and wraps to int32 before the
    // clamp — carried over verbatim from main.legacy.js:256. It matters for
    // fractional (2.7 -> 2) and for absurd (1e10 -> a wrapped int32) inputs,
    // both of which the clamp then pins into range.
    s = n.toFixed(Math.min(MAX_DECIMALS, Math.max(MIN_DECIMALS, f.decimals | 0)));
  } else if (setting.type === 'int') {
    s = String(Math.round(n));
  } else {
    // Default for float AND for any other type that reaches here: 2 places.
    s = Number(n).toFixed(2);
  }
  return (f.prefix || '') + s + (f.suffix || '');
}

/** Just the setting fields `optionLabel` reads. */
export type OptionLabelSource = Pick<Setting, 'options' | 'optionLabels'>;

/**
 * The display label for one enum/flags option (main.legacy.js:262-267).
 *
 * `optionLabels` is POSITIONAL — parallel to `options`. A short or holey label
 * array falls back to the option string itself, so a schema that labels only
 * some options still renders every one of them.
 */
export function optionLabel(setting: OptionLabelSource, opt: string): string {
  const opts = setting.options || [];
  const labels = setting.optionLabels || [];
  const idx = opts.indexOf(opt);
  // `!= null` (not a truthiness test): an intentional empty-string label is
  // honoured, only null/undefined falls back.
  if (idx >= 0) {
    const label = labels[idx];
    if (label != null) return label;
  }
  return opt;
}

/** Localiser shape: the shipped helper's `osfui.t` narrowed to what we need. */
export type Translate = (address: string, english: string) => string;

/**
 * The `requires` badge text (main.legacy.js:540-542).
 *
 * QUIRK, preserved: an UNRECOGNISED `requires` value is echoed back RAW, so
 * untrusted schema text reaches the badge. That is safe only because the badge
 * is built with `textContent` — a port that ever renders it as markup must
 * whitelist first.
 *
 * TWO deliberate divergences from the legacy object-literal lookup
 * (`{restart, reload, newGame}[value] || value`, main.legacy.js:540-542):
 *
 *  1. The literal's lookup walks the prototype chain, so `requires:"toString"`
 *     or `"constructor"` returned an inherited FUNCTION (truthy, so `|| value`
 *     never fired) and the badge rendered that function's source text. A switch
 *     cannot reproduce that, and it is untypeable as `string`. Not preserved:
 *     the output was garbage either way and nothing can depend on it.
 *  2. The literal called `tr` for all three kinds eagerly on every invocation;
 *     the switch calls it only for the matched one. `tr` is a pure catalogue
 *     lookup, so this is observable only by counting calls.
 */
export function requiresLabel(value: string, t?: Translate): string {
  const tr: Translate = t || ((_address, english) => english);
  switch (value) {
    case 'restart':
      return tr('restart', 'Restart');
    case 'reload':
      return tr('reloadUi', 'Reload UI');
    case 'newGame':
      return tr('newGame', 'New game');
    default:
      return value;
  }
}
