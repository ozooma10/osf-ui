import { describe, it, expect } from 'vitest';
import { canonicalName, NAME_ALIASES } from '@lib/keybinds/canonical';

describe('canonicalName', () => {
  it('folds every alias, case-insensitively', () => {
    for (const [alias, canonical] of Object.entries(NAME_ALIASES)) {
      expect(canonicalName(alias)).toBe(canonical);
      expect(canonicalName(alias.toUpperCase())).toBe(canonical);
      // Title case is how these actually arrive from schemas.
      expect(canonicalName(alias[0]!.toUpperCase() + alias.slice(1))).toBe(canonical);
      // Mixed case still folds — the lookup is on toLowerCase().
      expect(canonicalName(alias.slice(0, 1) + alias.slice(1).toUpperCase())).toBe(canonical);
    }
  });

  it('folds the documented spellings to the documented canonicals', () => {
    expect(canonicalName('Tilde')).toBe('Grave');
    expect(canonicalName('Backtick')).toBe('Grave');
    expect(canonicalName('Console')).toBe('Grave');
    expect(canonicalName('Return')).toBe('Enter');
  });

  it('uppercases a single lowercase letter', () => {
    expect(canonicalName('a')).toBe('A');
    expect(canonicalName('z')).toBe('Z');
    expect(canonicalName('f')).toBe('F');
  });

  it('leaves an already-uppercase letter alone (passthrough, not uppercasing)', () => {
    expect(canonicalName('A')).toBe('A');
    expect(canonicalName('Z')).toBe('Z');
  });

  it('does not touch multi-character names', () => {
    // Two lowercase letters fail /^[a-z]$/ and are not an alias.
    expect(canonicalName('ab')).toBe('ab');
    expect(canonicalName('F10')).toBe('F10');
    expect(canonicalName('LShift')).toBe('LShift');
    expect(canonicalName('PageDown')).toBe('PageDown');
    expect(canonicalName('Grave')).toBe('Grave');
    expect(canonicalName('1')).toBe('1');
  });

  it('coerces falsy input to the empty string (String(name || ""))', () => {
    expect(canonicalName(undefined)).toBe('');
    expect(canonicalName(null)).toBe('');
    expect(canonicalName('')).toBe('');
    // `name || ""` means 0 becomes "", not "0".
    expect(canonicalName(0)).toBe('');
  });

  it('QUIRK: inherits Object.prototype members through the alias table', () => {
    // Pins a known bug: the alias lookup is a bare index into an object
    // literal, so a name lowercasing to a prototype member returns the
    // inherited function — truthy, despite the declared `string` return type.
    // Reachable, since the input is a mod-authored stored value. See the note
    // in canonical.ts. A failure here means the quirk was fixed; that is fine
    // if intended, not if it fell out of a refactor.
    // Only all-lowercase prototype members are reachable (lookup is on
    // s.toLowerCase()): "constructor" and "__proto__".
    expect(typeof (canonicalName('constructor') as unknown)).toBe('function');
    expect(canonicalName('Constructor') as unknown).toBe(Object.prototype.constructor);
    expect(canonicalName('__proto__') as unknown).toBe(Object.prototype);
    // ...and these are safe only because the lowercasing does not match.
    expect(canonicalName('toString')).toBe('toString');
    expect(canonicalName('valueOf')).toBe('valueOf');
    expect(canonicalName('hasOwnProperty')).toBe('hasOwnProperty');
    // Sanity: a plain object literal has no own or inherited "prototype".
    expect(canonicalName('prototype')).toBe('prototype');
  });

  it('does not fold a name that merely CONTAINS an alias', () => {
    expect(canonicalName('Tilde2')).toBe('Tilde2');
    expect(canonicalName('ReturnKey')).toBe('ReturnKey');
  });
});
