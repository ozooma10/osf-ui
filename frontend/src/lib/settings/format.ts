// Display formatting for numeric settings, enum options and the `requires`
// badge. The stored value is always the raw number; this is presentation only.

import type { Setting } from '@sdk';

/**
 * Hard bounds on `format.decimals`. Crash guard, not cosmetics: `toFixed`
 * throws RangeError outside its supported range, and an uncaught throw in the
 * row builder aborts `renderDetail` and blanks the whole detail pane, so a
 * schema typo (`decimals: 21`) would take out the mod page. 20 is also the
 * documented schema limit.
 */
export const MIN_DECIMALS = 0;
export const MAX_DECIMALS = 20;

export type NumberFormatSource = Pick<Setting, 'type' | 'format'>;

/**
 * Render a numeric setting's value for display. `value` is `unknown` because
 * the slider path passes the DOM input's `value` (a string) straight in and
 * relies on `Number(v) * scale` to coerce.
 */
export function formatNumber(setting: NumberFormatSource, value: unknown): string {
  const f = setting.format || {};
  // `typeof === "number"` not `??`: a non-numeric `scale` in untrusted schema
  // JSON falls back to 1 instead of producing NaN.
  const scale = typeof f.scale === 'number' ? f.scale : 1;
  const n = Number(value) * scale;
  let s: string;
  if (typeof f.decimals === 'number') {
    // `| 0` truncates toward zero and wraps to int32 before the clamp: handles
    // fractional (2.7 -> 2) and absurd (1e10 -> wrapped int32) inputs, which
    // the clamp then pins into range.
    s = n.toFixed(Math.min(MAX_DECIMALS, Math.max(MIN_DECIMALS, f.decimals | 0)));
  } else if (setting.type === 'int') {
    s = String(Math.round(n));
  } else {
    // Float, and any other type that reaches here: 2 places.
    s = Number(n).toFixed(2);
  }
  return (f.prefix || '') + s + (f.suffix || '');
}

export type OptionLabelSource = Pick<Setting, 'options' | 'optionLabels'>;

/**
 * Display label for one enum/flags option. `optionLabels` is positional,
 * parallel to `options`; a short or holey array falls back to the option string
 * itself, so a schema that labels only some options still renders all of them.
 */
export function optionLabel(setting: OptionLabelSource, opt: string): string {
  const opts = setting.options || [];
  const labels = setting.optionLabels || [];
  const idx = opts.indexOf(opt);
  // `!= null` rather than truthiness: an empty-string label is honoured, only
  // null/undefined falls back.
  if (idx >= 0) {
    const label = labels[idx];
    if (label != null) return label;
  }
  return opt;
}

/** Narrowed shape of the shipped `osfui.t` localiser. */
export type Translate = (address: string, english: string) => string;

/**
 * The `requires` badge text. An unrecognised value is echoed back raw, so
 * untrusted schema text reaches the badge; safe only because the badge is built
 * with `textContent`. Rendering it as markup would need a whitelist first.
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
