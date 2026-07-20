import { describe, it, expect } from 'vitest';
import type { Setting } from '@sdk';
import {
  HEX_RE,
  MAX_KEY_NAME_LEN,
  MAX_STRING_LEN,
  isSetting,
  normalizeValue,
} from '@lib/settings/normalize';

const setting = (s: Partial<Setting> & Pick<Setting, 'type'>): Setting =>
  ({ key: 'k', ...s }) as Setting;

describe('isSetting', () => {
  it('accepts the seven frozen base types', () => {
    for (const type of ['bool', 'int', 'float', 'enum', 'flags', 'string', 'key']) {
      expect(isSetting({ key: 'k', type })).toBe(true);
    }
  });
  it('rejects note/image/action blocks and junk', () => {
    expect(isSetting({ type: 'note', text: 'hi' })).toBe(false);
    expect(isSetting({ type: 'image', src: 'a.png' })).toBe(false);
    expect(isSetting({ type: 'action', command: 'a.b' })).toBe(false);
    expect(isSetting(null)).toBe(false);
    expect(isSetting('bool')).toBe(false);
    expect(isSetting({})).toBe(false);
  });
  it('rejects a type this host predates (rendered read-only, never committed)', () => {
    expect(isSetting({ key: 'k', type: 'colorRamp' })).toBe(false);
  });
});

describe('normalizeValue — bool', () => {
  it('accepts only a strict boolean', () => {
    expect(normalizeValue(setting({ type: 'bool' }), true)).toBe(true);
    expect(normalizeValue(setting({ type: 'bool' }), false)).toBe(false);
  });
  it('REFUSES truthy/falsy stand-ins rather than coercing them', () => {
    expect(normalizeValue(setting({ type: 'bool' }), 1)).toBeUndefined();
    expect(normalizeValue(setting({ type: 'bool' }), 0)).toBeUndefined();
    expect(normalizeValue(setting({ type: 'bool' }), 'true')).toBeUndefined();
    expect(normalizeValue(setting({ type: 'bool' }), null)).toBeUndefined();
  });
});

describe('normalizeValue — int / float', () => {
  const int = setting({ type: 'int', min: 0, max: 10 });
  const float = setting({ type: 'float', min: 0, max: 1 });

  it('clamps to min/max', () => {
    expect(normalizeValue(int, 99)).toBe(10);
    expect(normalizeValue(int, -99)).toBe(0);
    expect(normalizeValue(float, 2.5)).toBe(1);
    expect(normalizeValue(float, -2.5)).toBe(0);
  });

  it('CLAMPS FIRST then rounds — a sub-min fraction lands on min, not below', () => {
    const gated = setting({ type: 'int', min: 1, max: 10 });
    expect(normalizeValue(gated, 0.4)).toBe(1);
    // Round-then-clamp also lands on 1 here; the max boundary with a fraction
    // above it is the case that distinguishes the two orders.
    expect(normalizeValue(gated, 10.6)).toBe(10);
  });

  it('rounds int but leaves float precision alone', () => {
    expect(normalizeValue(setting({ type: 'int' }), 2.5)).toBe(3);
    expect(normalizeValue(setting({ type: 'int' }), 2.4)).toBe(2);
    expect(normalizeValue(setting({ type: 'float' }), 2.4)).toBe(2.4);
  });

  it('ignores non-numeric min/max instead of producing NaN', () => {
    const loose = { key: 'k', type: 'int', min: 'x', max: null } as unknown as Setting;
    expect(normalizeValue(loose, 7)).toBe(7);
  });

  it('refuses non-numbers and non-finite numbers', () => {
    expect(normalizeValue(int, '5')).toBeUndefined();
    expect(normalizeValue(int, true)).toBeUndefined();
    expect(normalizeValue(int, NaN)).toBeUndefined();
    expect(normalizeValue(int, Infinity)).toBeUndefined();
  });
});

describe('normalizeValue — enum', () => {
  const e = setting({ type: 'enum', options: ['low', 'high'] });
  it('accepts a declared option', () => {
    expect(normalizeValue(e, 'high')).toBe('high');
  });
  it('refuses an undeclared option, a non-string, and a missing options array', () => {
    expect(normalizeValue(e, 'ultra')).toBeUndefined();
    expect(normalizeValue(e, 1)).toBeUndefined();
    expect(normalizeValue(setting({ type: 'enum' }), 'low')).toBeUndefined();
  });
});

describe('normalizeValue — flags', () => {
  const f = setting({ type: 'flags', options: ['a', 'b', 'c'] });

  it('emits CANONICAL DECLARED ORDER, not the incoming order', () => {
    expect(normalizeValue(f, ['c', 'a'])).toEqual(['a', 'c']);
    expect(normalizeValue(f, ['b', 'c', 'a'])).toEqual(['a', 'b', 'c']);
  });

  it('dedupes', () => {
    expect(normalizeValue(f, ['b', 'b', 'b'])).toEqual(['b']);
  });

  it('drops unknown options and non-string junk instead of refusing', () => {
    expect(normalizeValue(f, ['a', 'zzz', 3, null, 'c'])).toEqual(['a', 'c']);
  });

  it('accepts the empty selection', () => {
    expect(normalizeValue(f, [])).toEqual([]);
  });

  it('refuses a non-array value or a schema with no options', () => {
    expect(normalizeValue(f, 'a')).toBeUndefined();
    expect(normalizeValue(setting({ type: 'flags' }), ['a'])).toBeUndefined();
  });

  it('skips non-string declared options', () => {
    const odd = { key: 'k', type: 'flags', options: ['a', 7, 'b'] } as unknown as Setting;
    expect(normalizeValue(odd, ['b', 7, 'a'])).toEqual(['a', 'b']);
  });
});

describe('normalizeValue — string', () => {
  it('caps at 256 by default', () => {
    const long = 'x'.repeat(400);
    expect(normalizeValue(setting({ type: 'string' }), long)).toHaveLength(MAX_STRING_LEN);
  });

  it('honours a smaller maxLength', () => {
    expect(normalizeValue(setting({ type: 'string', maxLength: 4 }), 'abcdefg')).toBe('abcd');
  });

  it('never lets maxLength EXCEED the store-wide cap', () => {
    const long = 'x'.repeat(400);
    expect(normalizeValue(setting({ type: 'string', maxLength: 4096 }), long)).toHaveLength(256);
  });

  it('QUIRK: maxLength 0 is treated as unset because of `|| 256`', () => {
    expect(normalizeValue(setting({ type: 'string', maxLength: 0 }), 'abc')).toBe('abc');
  });

  it('QUIRK: a negative maxLength chops from the END (Math.min(256, -3) = -3)', () => {
    // Native ignores maxLength <= 0; the renderer formula does not. Divergence
    // is preserved, not corrected.
    expect(normalizeValue(setting({ type: 'string', maxLength: -3 }), 'abcdef')).toBe('abc');
  });

  it('QUIRK: a FRACTIONAL maxLength truncates (native requires an integer)', () => {
    // SettingsStore.cpp gates on is_number_integer(), so native ignores 2.5;
    // the renderer formula slices at 2. Same divergence as the negative case.
    const frac = { key: 'k', type: 'string', maxLength: 2.5 } as unknown as Setting;
    expect(normalizeValue(frac, 'abcdef')).toBe('ab');
  });

  it('holds a color widget to #rrggbb / #rrggbbaa and REFUSES anything else', () => {
    const c = setting({ type: 'string', widget: 'color' });
    expect(normalizeValue(c, '#5aa9b8')).toBe('#5aa9b8');
    expect(normalizeValue(c, '#5aa9b8ff')).toBe('#5aa9b8ff');
    expect(normalizeValue(c, '#abc')).toBeUndefined();
    expect(normalizeValue(c, 'red')).toBeUndefined();
    expect(HEX_RE.test('#5AA9B8')).toBe(true);
  });

  it('refuses a non-string', () => {
    expect(normalizeValue(setting({ type: 'string' }), 5)).toBeUndefined();
  });
});

describe('normalizeValue — key', () => {
  it('caps a key name at 16 characters', () => {
    const long = 'K'.repeat(40);
    expect(normalizeValue(setting({ type: 'key' }), long)).toHaveLength(MAX_KEY_NAME_LEN);
    expect(normalizeValue(setting({ type: 'key' }), 'F10')).toBe('F10');
  });

  it('REFUSES "" unless the schema opted into allowUnbound', () => {
    // A blank must not clobber a working binding by accident.
    expect(normalizeValue(setting({ type: 'key' }), '')).toBeUndefined();
    expect(normalizeValue(setting({ type: 'key', allowUnbound: true }), '')).toBe('');
  });

  it('requires allowUnbound to be strictly true', () => {
    const loose = { key: 'k', type: 'key', allowUnbound: 1 } as unknown as Setting;
    expect(normalizeValue(loose, '')).toBeUndefined();
  });

  it('refuses a non-string', () => {
    expect(normalizeValue(setting({ type: 'key' }), 112)).toBeUndefined();
  });
});

describe('normalizeValue — unknown type', () => {
  it('refuses outright (the store serves it read-only)', () => {
    const future = { key: 'k', type: 'colorRamp' } as unknown as Setting;
    expect(normalizeValue(future, 'anything')).toBeUndefined();
    expect(normalizeValue(future, 1)).toBeUndefined();
  });
});
