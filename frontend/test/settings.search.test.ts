import { describe, it, expect } from 'vitest';
import type { Setting } from '@sdk';
import type { ModRecord, RailEntry, ViewRecord } from '@lib/settings/rail';
import { railEntries } from '@lib/settings/rail';
import { railMatches, searchResults } from '@lib/settings/search';

const setting = (o: Partial<Setting> & Pick<Setting, 'key' | 'type'>): Setting => o as Setting;

const demo: ModRecord = {
  id: 'acme.demo',
  title: 'Ship Works',
  schema: {
    groups: [
      {
        label: 'Flight',
        settings: [
          setting({ key: 'boost', label: 'Afterburner', type: 'bool' }),
          setting({ key: 'trimOnly', type: 'int' }), // no label -> key is searched
        ],
      },
      {
        // Unlabelled group.
        settings: [setting({ key: 'gauge', label: 'Fuel gauge', type: 'bool' })],
      },
    ],
  },
};

const other: ModRecord = {
  id: 'acme.atlas',
  title: 'Star Atlas',
  schema: { groups: [{ label: 'Map', settings: [setting({ key: 'grid', label: 'Grid', type: 'bool' })] }] },
};

const entryFor = (mods: ModRecord[], views: ViewRecord[], id: string): RailEntry =>
  railEntries(mods, views).find((e) => e.id === id)!;

describe('railMatches', () => {
  const entry = entryFor([demo], [{ id: 'acme.demo/hud', title: 'Cockpit HUD', mod: 'acme.demo' }], 'acme.demo');

  it('matches everything on an empty query', () => {
    expect(railMatches(entry, '')).toBe(true);
  });

  it('matches the entry title', () => {
    expect(railMatches(entry, 'ship')).toBe(true);
  });

  it('matches an attached VIEW title', () => {
    expect(railMatches(entry, 'cockpit')).toBe(true);
  });

  it('matches a setting label', () => {
    expect(railMatches(entry, 'afterburner')).toBe(true);
  });

  it('matches a setting KEY when it has no label', () => {
    expect(railMatches(entry, 'trimonly')).toBe(true);
  });

  it('does not match an unrelated query', () => {
    expect(railMatches(entry, 'quasar')).toBe(false);
  });

  it('does NOT match the mod id (only titles and labels are searched)', () => {
    expect(railMatches(entry, 'acme')).toBe(false);
  });

  it('expects a PRE-LOWERCASED query — mixed case never matches', () => {
    expect(railMatches(entry, 'Ship')).toBe(false);
  });

  it('unlike the cross-mod scan, matches a NON-setting item label too', () => {
    const withAction: ModRecord = {
      id: 'x',
      title: 'X',
      schema: {
        groups: [{ settings: [{ type: 'action', key: 'purge', label: 'Purge caches', command: 'x.purge' }] }],
      },
    };
    expect(railMatches(entryFor([withAction], [], 'x'), 'purge')).toBe(true);
  });

  it('tolerates a view-only entry with no mod', () => {
    const e = entryFor([], [{ id: 'solo/panel', title: 'Solo Panel', kind: 'menu' }], 'view:solo/panel');
    expect(railMatches(e, 'solo')).toBe(true);
    expect(railMatches(e, 'nope')).toBe(false);
  });
});

describe('searchResults — cross-mod scan', () => {
  it('matches a setting label and builds a "mod › group" breadcrumb', () => {
    const hits = searchResults([demo, other], 'afterburner');
    expect(hits).toHaveLength(1);
    expect(hits[0]).toMatchObject({
      modId: 'acme.demo',
      key: 'boost',
      label: 'Afterburner',
      groupLabel: 'Flight',
      breadcrumb: 'Ship Works › Flight',
    });
  });

  it('drops the separator for an unlabelled group', () => {
    const hits = searchResults([demo], 'fuel');
    expect(hits[0]?.breadcrumb).toBe('Ship Works');
    expect(hits[0]?.groupLabel).toBe('');
  });

  it('lists EVERY setting of a mod whose TITLE matches', () => {
    const hits = searchResults([demo, other], 'ship works');
    expect(hits.map((h) => h.key)).toEqual(['boost', 'trimOnly', 'gauge']);
  });

  it('uses the key as the label when none is authored', () => {
    const hits = searchResults([demo], 'trimonly');
    expect(hits[0]?.label).toBe('trimOnly');
  });

  it('keeps ONLY isSetting items — notes, images and actions are skipped', () => {
    const mixed: ModRecord = {
      id: 'x',
      title: 'X',
      schema: {
        groups: [
          {
            label: 'G',
            settings: [
              { type: 'note', text: 'gamma note' },
              { type: 'image', src: 'gamma.png', caption: 'gamma' },
              { type: 'action', key: 'gammaRun', label: 'gamma action', command: 'x.gammaRun' },
              setting({ key: 'gammaSetting', label: 'gamma setting', type: 'bool' }),
            ],
          },
        ],
      },
    };
    expect(searchResults([mixed], 'gamma').map((h) => h.key)).toEqual(['gammaSetting']);
  });

  it('drops settings whose key is missing or empty', () => {
    const broken: ModRecord = {
      id: 'x',
      title: 'X',
      schema: {
        groups: [
          {
            settings: [
              { type: 'bool', label: 'delta keyless' } as unknown as Setting,
              setting({ key: '', label: 'delta empty', type: 'bool' }),
              { key: 7, type: 'bool', label: 'delta numeric' } as unknown as Setting,
              setting({ key: 'ok', label: 'delta ok', type: 'bool' }),
            ],
          },
        ],
      },
    };
    expect(searchResults([broken], 'delta').map((h) => h.key)).toEqual(['ok']);
  });

  it('returns nothing for a query that matches neither label nor mod title', () => {
    expect(searchResults([demo, other], 'quasar')).toEqual([]);
  });

  it('tolerates mods with no schema at all', () => {
    expect(searchResults([{ id: 'bare' }], 'anything')).toEqual([]);
  });

  it('preserves mod, then group, then declaration order', () => {
    const hits = searchResults([demo, other], '');
    expect(hits.map((h) => h.key)).toEqual(['boost', 'trimOnly', 'gauge', 'grid']);
  });
});
