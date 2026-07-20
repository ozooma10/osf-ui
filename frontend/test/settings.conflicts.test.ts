import { describe, it, expect } from 'vitest';
import type { Setting } from '@sdk';
import type { ModRecord } from '@lib/settings/rail';
import { applyConflictUpdate } from '@lib/settings/conflicts';
import type { ConflictEntry } from '@lib/settings/conflicts';

const key = (k: string, over: Partial<Setting> = {}): Setting =>
  ({ key: k, type: 'key', ...over }) as Setting;

function model(): ModRecord[] {
  return [
    {
      id: 'osfui',
      title: 'OSF UI',
      schema: { groups: [{ settings: [key('toggleKey'), { type: 'note', text: 'n' }] }] },
      values: { toggleKey: 'F10' },
    },
    {
      id: 'acme.demo',
      title: 'Demo',
      schema: {
        groups: [
          { settings: [key('open'), key('close'), { key: 'vol', type: 'int' } as Setting] },
        ],
      },
      values: {},
    },
  ];
}

const find = (mods: ModRecord[], modId: string, k: string): Setting | undefined =>
  mods
    .find((m) => m.id === modId)
    ?.schema?.groups?.flatMap((g) => g.settings || [])
    .find((s) => (s as Setting).key === k) as Setting | undefined;

describe('applyConflictUpdate — purity', () => {
  it('does NOT mutate the input model', () => {
    const before = model();
    const snapshot = JSON.stringify(before);
    applyConflictUpdate(before, 'osfui', 'toggleKey', [
      { mod: 'acme.demo', key: 'open', title: 'Demo' },
    ]);
    expect(JSON.stringify(before)).toBe(snapshot);
  });

  it('returns a NEW array with new setting objects where anything changed', () => {
    const before = model();
    const after = applyConflictUpdate(before, 'osfui', 'toggleKey', [
      { mod: 'acme.demo', key: 'open', title: 'Demo' },
    ]);
    expect(after).not.toBe(before);
    expect(find(after, 'osfui', 'toggleKey')).not.toBe(find(before, 'osfui', 'toggleKey'));
  });

  it('leaves untouched mods identical by reference', () => {
    const before = model();
    const after = applyConflictUpdate(before, 'osfui', 'toggleKey', []);
    // Nothing had conflicts, so nothing changes anywhere.
    expect(after[0]).toBe(before[0]);
    expect(after[1]).toBe(before[1]);
  });
});

describe('applyConflictUpdate — symmetric mirroring', () => {
  const partner: ConflictEntry = { mod: 'acme.demo', key: 'open', title: 'Demo' };

  it('sets the changed setting\'s list verbatim', () => {
    const after = applyConflictUpdate(model(), 'osfui', 'toggleKey', [partner]);
    expect(find(after, 'osfui', 'toggleKey')?.conflicts).toEqual([partner]);
  });

  it('mirrors the SELF entry onto every named partner', () => {
    const after = applyConflictUpdate(model(), 'osfui', 'toggleKey', [partner]);
    expect(find(after, 'acme.demo', 'open')?.conflicts).toEqual([
      { mod: 'osfui', key: 'toggleKey', title: 'OSF UI' },
    ]);
  });

  it('uses titleOf() for the self entry, falling back to the schema title then the id', () => {
    const mods = model();
    delete mods[0]!.title;
    mods[0]!.schema!.title = 'Framework';
    const after = applyConflictUpdate(mods, 'osfui', 'toggleKey', [partner]);
    expect(find(after, 'acme.demo', 'open')?.conflicts?.[0]?.title).toBe('Framework');
  });

  it('REMOVES a stale entry from a former partner no longer in the list', () => {
    const mods = model();
    find(mods, 'acme.demo', 'open')!.conflicts = [
      { mod: 'osfui', key: 'toggleKey', title: 'OSF UI' },
    ];
    const after = applyConflictUpdate(mods, 'osfui', 'toggleKey', []);
    // Empty list drops the property; that is the documented "no collisions"
    // encoding, not an empty array.
    expect(find(after, 'acme.demo', 'open')).not.toHaveProperty('conflicts');
    expect(find(after, 'osfui', 'toggleKey')).not.toHaveProperty('conflicts');
  });

  it('leaves entries pointing at UNRELATED settings alone', () => {
    const mods = model();
    find(mods, 'acme.demo', 'open')!.conflicts = [
      { mod: 'other.mod', key: 'x', title: 'Other' },
      { mod: 'osfui', key: 'toggleKey', title: 'OSF UI' },
    ];
    const after = applyConflictUpdate(mods, 'osfui', 'toggleKey', []);
    expect(find(after, 'acme.demo', 'open')?.conflicts).toEqual([
      { mod: 'other.mod', key: 'x', title: 'Other' },
    ]);
  });

  it('mirrors onto a SIBLING setting in the same mod as the changed one', () => {
    const after = applyConflictUpdate(model(), 'acme.demo', 'open', [
      { mod: 'acme.demo', key: 'close', title: 'Demo' },
    ]);
    expect(find(after, 'acme.demo', 'close')?.conflicts).toEqual([
      { mod: 'acme.demo', key: 'open', title: 'Demo' },
    ]);
  });

  it('never touches non-key settings', () => {
    const after = applyConflictUpdate(model(), 'osfui', 'toggleKey', [
      { mod: 'acme.demo', key: 'vol', title: 'Demo' },
    ]);
    // "vol" is an int, so even though the push names it, it gets no badge.
    expect(find(after, 'acme.demo', 'vol')).not.toHaveProperty('conflicts');
  });

  it('needs no touch-up for a reserved "@game" partner (it owns no setting)', () => {
    const gameEntry: ConflictEntry = { mod: '@game', key: 'Quicksave', title: 'Starfield (Quicksave)' };
    const after = applyConflictUpdate(model(), 'osfui', 'toggleKey', [gameEntry]);
    expect(find(after, 'osfui', 'toggleKey')?.conflicts).toEqual([gameEntry]);
    expect(after.some((m) => m.id === '@game')).toBe(false);
  });

  it('returns the model unchanged when the mod is not loaded', () => {
    const before = model();
    expect(applyConflictUpdate(before, 'ghost.mod', 'k', [])).toBe(before);
  });

  it('tolerates a mod with no schema/groups', () => {
    const mods: ModRecord[] = [{ id: 'osfui', title: 'OSF UI' }, ...model().slice(1)];
    const after = applyConflictUpdate(mods, 'osfui', 'toggleKey', []);
    expect(after[0]).toBe(mods[0]);
  });
});

describe('applyConflictUpdate — duplicate-key quirk', () => {
  it('applies the push to the FIRST matching setting only, leaving the second stale', () => {
    // Malformed schema declaring the same key twice: the list is set on
    // findSettingInMod's first hit and same-key settings are skipped in the
    // mirror loop, so the duplicate keeps whatever it had.
    const mods: ModRecord[] = [
      {
        id: 'acme.demo',
        title: 'Demo',
        schema: {
          groups: [
            { settings: [key('open'), key('open', { conflicts: [{ mod: 'z', key: 'z', title: 'Z' }] })] },
          ],
        },
      },
    ];
    const after = applyConflictUpdate(mods, 'acme.demo', 'open', [
      { mod: 'w', key: 'w', title: 'W' },
    ]);
    const items = after[0]!.schema!.groups![0]!.settings as Setting[];
    expect(items[0]?.conflicts).toEqual([{ mod: 'w', key: 'w', title: 'W' }]);
    expect(items[1]?.conflicts).toEqual([{ mod: 'z', key: 'z', title: 'Z' }]);
  });
});
