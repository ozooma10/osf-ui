import { describe, it, expect } from 'vitest';
import { makeLabeler } from '@lib/keybinds/labels';
import { buildModel } from '@lib/keybinds/model';
import type { SettingsData } from '@sdk';

const keyboard = (labels: Record<string, string>): SettingsData['keyboard'] =>
  ({ layout: 'de-DE', labels }) as SettingsData['keyboard'];

describe('makeLabeler', () => {
  it('returns the label for a mapped name and undefined for the rest', () => {
    const labeler = makeLabeler(keyboard({ Semicolon: 'Ö', Grave: '^' }));
    expect(labeler('Semicolon')).toBe('Ö');
    expect(labeler('Grave')).toBe('^');
    expect(labeler('W')).toBeUndefined();
  });

  it('degrades to the no-map labeler on absent/hostile shapes', () => {
    expect(makeLabeler(undefined)('Semicolon')).toBeUndefined();
    expect(makeLabeler(null)('Semicolon')).toBeUndefined();
    expect(makeLabeler({ layout: '', labels: null } as unknown as SettingsData['keyboard'])('A')).toBeUndefined();
  });

  it('is own-property-guarded and ignores non-string/empty labels', () => {
    const labeler = makeLabeler(keyboard({ Empty: '', Num: 7 as unknown as string }));
    // Inherited Object.prototype members must never surface as labels.
    expect(labeler('constructor')).toBeUndefined();
    expect(labeler('__proto__')).toBeUndefined();
    expect(labeler('hasOwnProperty')).toBeUndefined();
    expect(labeler('Empty')).toBeUndefined();
    expect(labeler('Num')).toBeUndefined();
  });
});

describe('buildModel keyLabel plumbing', () => {
  it('fills keyLabel from the labeler, name-fallback when unmapped', () => {
    const labeler = makeLabeler(keyboard({ Semicolon: 'Ö' }));
    const rows = buildModel(
      [
        {
          id: 'm',
          title: 'M',
          values: { a: 'Semicolon', b: 'F5' },
          schema: {
            groups: [
              {
                settings: [
                  { key: 'a', type: 'key' },
                  { key: 'b', type: 'key' },
                ],
              },
            ],
          },
        },
      ] as never,
      [{
        event: 'QuickSave', label: 'Quicksave', category: 'MainGameplay',
        context: { id: 0, name: 'MainGameplay', order: 0 }, classification: 'core',
        modes: { definite: ['onFoot'], possible: [] }, sortIndex: 0, required: false,
        bindings: [{ slot: 'main', key: 'Semicolon', chord: ['Semicolon'], unbound: false }],
      }],
      undefined,
      labeler,
    );
    expect(rows.map((r) => [r.name, r.keyLabel])).toEqual([
      ['Semicolon', 'Ö'],
      ['F5', 'F5'],
      ['Semicolon', 'Ö'],
    ]);
  });

  it('labels through the canonical fold: an aliased stored value still maps', () => {
    const labeler = makeLabeler(keyboard({ Grave: '^' }));
    const rows = buildModel(
      [
        {
          id: 'm',
          title: 'M',
          values: { a: 'Tilde' },
          schema: { groups: [{ settings: [{ key: 'a', type: 'key' }] }] },
        },
      ] as never,
      [],
      undefined,
      labeler,
    );
    expect(rows[0]!.name).toBe('Grave');
    expect(rows[0]!.keyLabel).toBe('^');
  });
});
