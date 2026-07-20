import { describe, it, expect } from 'vitest';
import { keyOrder, compareBindings } from '@lib/keybinds/sort';
import type { BindingRow } from '@lib/keybinds/model';

function row(name: string, owner: string): BindingRow {
  return {
    kind: 'mod',
    mod: 'm',
    key: 'k',
    label: 'l',
    owner,
    name,
    contextId: 'gameplay',
    contextLabel: 'Gameplay',
    blocksGameplay: false,
  };
}

describe('keyOrder', () => {
  it('prefixes F-keys with 0 and zero-pads to 3 digits', () => {
    expect(keyOrder('F1')).toBe('0001');
    expect(keyOrder('F2')).toBe('0002');
    expect(keyOrder('F10')).toBe('0010');
    expect(keyOrder('F24')).toBe('0024');
  });

  it('prefixes everything else with 1 and leaves the name alone', () => {
    expect(keyOrder('Insert')).toBe('1Insert');
    expect(keyOrder('Grave')).toBe('1Grave');
    expect(keyOrder('A')).toBe('1A');
    // "F" alone has no digits, so it is not an F-key.
    expect(keyOrder('F')).toBe('1F');
    // A suffix after the digits also fails the anchored regex.
    expect(keyOrder('F10x')).toBe('1F10x');
  });

  it('orders F2 < F10 < Insert', () => {
    const names = ['Insert', 'F10', 'F2'];
    names.sort((a, b) => keyOrder(a).localeCompare(keyOrder(b)));
    expect(names).toEqual(['F2', 'F10', 'Insert']);
  });

  it('would NOT order F2 before F10 without the transform (regression guard)', () => {
    expect('F10'.localeCompare('F2')).toBeLessThan(0);
    expect(keyOrder('F10').localeCompare(keyOrder('F2'))).toBeGreaterThan(0);
  });
});

describe('compareBindings', () => {
  it('sorts by key order first', () => {
    const rows = [row('Insert', 'Zeta'), row('F10', 'Zeta'), row('F2', 'Zeta')];
    expect([...rows].sort(compareBindings).map((r) => r.name)).toEqual(['F2', 'F10', 'Insert']);
  });

  it('falls back to owner localeCompare on the same key', () => {
    const rows = [row('F5', 'Zeta Mod'), row('F5', 'Alpha Mod'), row('F5', 'Middle Mod')];
    expect([...rows].sort(compareBindings).map((r) => r.owner)).toEqual([
      'Alpha Mod',
      'Middle Mod',
      'Zeta Mod',
    ]);
  });

  it('keeps input order for identical key and owner (stable sort)', () => {
    // Identity assertions, not toEqual: a and b are structurally identical, so
    // toEqual would pass even if the comparator reordered them.
    const a = row('F5', 'Same');
    const b = row('F5', 'Same');
    expect(compareBindings(a, b)).toBe(0);
    const forward = [a, b].sort(compareBindings);
    expect(forward[0]).toBe(a);
    expect(forward[1]).toBe(b);
    const reverse = [b, a].sort(compareBindings);
    expect(reverse[0]).toBe(b);
    expect(reverse[1]).toBe(a);
  });

  it('never orders by label - only key name then owner', () => {
    // There is no third comparison key, so two rows from the same owner on the
    // same key keep model order regardless of their labels.
    const zed = { ...row('F5', 'Same'), label: 'zzz' };
    const abc = { ...row('F5', 'Same'), label: 'aaa' };
    const sorted = [zed, abc].sort(compareBindings);
    expect(sorted[0]?.label).toBe('zzz');
  });
});
