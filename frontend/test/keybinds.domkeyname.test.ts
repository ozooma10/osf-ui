import { describe, it, expect } from 'vitest';
import { domKeyName } from '@lib/keybinds/domKeyName';

const k = (key: string) => domKeyName({ key });

describe('domKeyName', () => {
  it('passes F1-F24 through verbatim', () => {
    for (let i = 1; i <= 24; ++i) expect(k(`F${i}`)).toBe(`F${i}`);
  });

  it('rejects F-keys outside the native range', () => {
    expect(k('F0')).toBe('');
    expect(k('F25')).toBe('');
  });

  it('uppercases letters of EITHER case', () => {
    expect(k('a')).toBe('A');
    // Unlike canonicalName, the /i flag means an uppercase letter also takes
    // this branch (same result, different path).
    expect(k('A')).toBe('A');
    expect(k('z')).toBe('Z');
  });

  it('passes digits through', () => {
    for (const d of '0123456789') expect(k(d)).toBe(d);
  });

  it('maps the named keys', () => {
    expect(k(' ')).toBe('Space');
    expect(k('Enter')).toBe('Enter');
    expect(k('Tab')).toBe('Tab');
    expect(k('Backspace')).toBe('Backspace');
    expect(k('Insert')).toBe('Insert');
    expect(k('Delete')).toBe('Delete');
    expect(k('Home')).toBe('Home');
    expect(k('End')).toBe('End');
    expect(k('PageUp')).toBe('PageUp');
    expect(k('PageDown')).toBe('PageDown');
    expect(k('`')).toBe('Grave');
  });

  it('strips the Arrow prefix', () => {
    expect(k('ArrowUp')).toBe('Up');
    expect(k('ArrowDown')).toBe('Down');
    expect(k('ArrowLeft')).toBe('Left');
    expect(k('ArrowRight')).toBe('Right');
  });

  it('returns "" for unmapped keys, which the capture path treats as cancel', () => {
    expect(k('Escape')).toBe('');
    // Modifiers are drawn on the board but are NOT resolvable from e.key,
    // since DOM cannot distinguish the left and right sides here.
    expect(k('Shift')).toBe('');
    expect(k('Control')).toBe('');
    expect(k('Alt')).toBe('');
    expect(k('CapsLock')).toBe('');
    expect(k('-')).toBe('');
    expect(k('')).toBe('');
  });

  it('does not inherit Object.prototype members through the named table', () => {
    expect(k('constructor')).toBe('');
    expect(k('toString')).toBe('');
  });
});
