
import type { Setting } from '@sdk';

export const MIN_DECIMALS = 0;
export const MAX_DECIMALS = 20;

export type NumberFormatSource = Pick<Setting, 'type' | 'format'>;

export function formatNumber(setting: NumberFormatSource, value: unknown): string {
  const f = setting.format || {};
  const scale = typeof f.scale === 'number' ? f.scale : 1;
  const n = Number(value) * scale;
  let s: string;
  if (typeof f.decimals === 'number') {
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

export function optionLabel(setting: OptionLabelSource, opt: string): string {
  const opts = setting.options || [];
  const labels = setting.optionLabels || [];
  const idx = opts.indexOf(opt);
  if (idx >= 0) {
    const label = labels[idx];
    if (label != null) return label;
  }
  return opt;
}

/** Narrowed shape of the shipped `osfui.t` localiser. */
export type Translate = (address: string, english: string) => string;

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
