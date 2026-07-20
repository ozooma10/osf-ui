import { describe, it, expect } from 'vitest';
import type { Setting } from '@sdk';
import { formatNumber, optionLabel, requiresLabel } from '@lib/settings/format';

const num = (o: Partial<Setting>): Pick<Setting, 'type' | 'format'> =>
  ({ type: 'float', ...o }) as Setting;

describe('formatNumber — decimals clamp to [0,20]', () => {
  it('honours a legal decimals value', () => {
    expect(formatNumber(num({ format: { decimals: 3 } }), 1.23456)).toBe('1.235');
    expect(formatNumber(num({ format: { decimals: 0 } }), 1.6)).toBe('2');
  });

  it('CLAMPS above 20 instead of throwing RangeError', () => {
    // Keep schema mistakes bounded; an uncaught formatter error would abort
    // renderDetail and blank the entire mod page.
    expect(() => formatNumber(num({ format: { decimals: 21 } }), 1)).not.toThrow();
    expect(formatNumber(num({ format: { decimals: 21 } }), 1)).toBe((1).toFixed(20));
    expect(formatNumber(num({ format: { decimals: 1000 } }), 1)).toBe((1).toFixed(20));
  });

  it('CLAMPS below 0', () => {
    expect(() => formatNumber(num({ format: { decimals: -5 } }), 1.6)).not.toThrow();
    expect(formatNumber(num({ format: { decimals: -5 } }), 1.6)).toBe('2');
  });

  it('truncates a fractional decimals with `| 0` before clamping', () => {
    expect(formatNumber(num({ format: { decimals: 2.9 } }), 1.239)).toBe('1.24');
  });

  it('survives an absurd decimals that `| 0` wraps to a negative int32', () => {
    // 2**31 wraps to -2147483648, which the lower clamp pins to 0.
    expect(() => formatNumber(num({ format: { decimals: 2 ** 31 } }), 1.6)).not.toThrow();
    expect(formatNumber(num({ format: { decimals: 2 ** 31 } }), 1.6)).toBe('2');
  });
});

describe('formatNumber — scale, prefix, suffix, type defaults', () => {
  it('defaults scale to 1 and ignores a non-numeric scale', () => {
    expect(formatNumber(num({}), 1.5)).toBe('1.50');
    const bad = { type: 'float', format: { scale: 'x' } } as unknown as Setting;
    expect(formatNumber(bad, 1.5)).toBe('1.50');
  });

  it('multiplies by scale before formatting', () => {
    expect(formatNumber(num({ format: { scale: 100, decimals: 0, suffix: '%' } }), 0.25)).toBe('25%');
  });

  it('renders an int with no decimals as a rounded integer', () => {
    expect(formatNumber(num({ type: 'int' }), 3.6)).toBe('4');
  });

  it('renders a float with no decimals at 2 places', () => {
    expect(formatNumber(num({ type: 'float' }), 3)).toBe('3.00');
  });

  it('wraps with prefix and suffix', () => {
    expect(formatNumber(num({ type: 'int', format: { prefix: '×', suffix: ' u' } }), 2)).toBe('×2 u');
  });

  it('coerces a STRING value, as the slider path passes it', () => {
    // The slider path hands the DOM input's `value` straight in.
    expect(formatNumber(num({ type: 'int' }), '7')).toBe('7');
  });
});

describe('optionLabel', () => {
  const setting = { options: ['low', 'mid', 'high'], optionLabels: ['Low', 'Mid'] };

  it('uses the parallel label when present', () => {
    expect(optionLabel(setting, 'low')).toBe('Low');
    expect(optionLabel(setting, 'mid')).toBe('Mid');
  });

  it('falls back to the option itself when the label array is short', () => {
    expect(optionLabel(setting, 'high')).toBe('high');
  });

  it('falls back for an option that is not declared at all', () => {
    expect(optionLabel(setting, 'ultra')).toBe('ultra');
  });

  it('honours an intentional empty-string label (`!= null`, not truthiness)', () => {
    expect(optionLabel({ options: ['a'], optionLabels: [''] }, 'a')).toBe('');
  });

  it('falls back through a holey label array', () => {
    const holey = { options: ['a', 'b'], optionLabels: [undefined, 'B'] } as unknown as {
      options: string[];
      optionLabels: string[];
    };
    expect(optionLabel(holey, 'a')).toBe('a');
    expect(optionLabel(holey, 'b')).toBe('B');
  });

  it('tolerates missing options/optionLabels entirely', () => {
    expect(optionLabel({}, 'x')).toBe('x');
  });
});

describe('requiresLabel', () => {
  it('maps the three known kinds', () => {
    expect(requiresLabel('restart')).toBe('Restart');
    expect(requiresLabel('reload')).toBe('Reload UI');
    expect(requiresLabel('newGame')).toBe('New game');
  });

  it('routes through an injected localiser with the legacy addresses', () => {
    const seen: string[] = [];
    const t = (address: string, english: string) => {
      seen.push(address);
      return english.toUpperCase();
    };
    expect(requiresLabel('reload', t)).toBe('RELOAD UI');
    expect(seen).toEqual(['reloadUi']);
  });

  it('QUIRK: echoes an unrecognised value back RAW (untrusted schema text)', () => {
    expect(requiresLabel('reboot')).toBe('reboot');
    expect(requiresLabel('<b>x</b>')).toBe('<b>x</b>');
  });
});
