import { describe, it, expect } from 'vitest';
import { buildModel } from '@lib/keybinds/model';
import { holdersOf } from '@lib/keybinds/conflicts';
import type { ModEntry } from '@lib/keybinds/model';
import type { KeybindingsData } from '@sdk';

/** Builds a mod entry holding only the fields buildModel reads. */
function mod(entry: {
  id: string;
  title?: string;
  settings: Array<Record<string, unknown>>;
  values?: Record<string, unknown>;
  inputContexts?: Array<Record<string, unknown>>;
}): ModEntry {
  return {
    id: entry.id,
    title: entry.title ?? '',
    schema: {
      ...(entry.inputContexts ? { inputContexts: entry.inputContexts } : {}),
      groups: [{ settings: entry.settings }],
    },
    values: entry.values ?? {},
  } as unknown as ModEntry;
}

function gameAction(event: string, label: string, key: string): KeybindingsData['actions'][number] {
  return {
    event, label, category: 'Gameplay',
    context: { id: 0, name: 'MainGameplay', order: 0 }, classification: 'core',
    modes: { definite: ['onFoot', 'ship', 'vehicle', 'zeroG'], possible: [] },
    sortIndex: 0, required: false,
    bindings: [{ slot: 'main', key, chord: [key], unbound: false }],
  };
}

// Input-context resolution itself is @lib/settings/inputContext (covered in
// settings.inputcontext.test.ts); these assert the keybinds model delegates to
// it and localizes the implicit-context label.
describe('buildModel input contexts', () => {
  const contexts = [{ id: 'menu', label: 'Menu', blocksGameplay: true }];

  it('resolves a declared context through the shared resolver', () => {
    const rows = buildModel([mod({
      id: 'acme.widgets',
      settings: [{ type: 'key', key: 'openMenu', inputContext: 'menu' }],
      values: { openMenu: 'F7' },
      inputContexts: contexts,
    })], []);
    expect(rows[0]).toMatchObject({
      contextId: 'menu',
      contextLabel: 'Menu',
      blocksGameplay: true,
    });
  });

  it('localizes implicit mod gameplay while preserving the engine context name', () => {
    const translate = (address: string, english: string) =>
      address === 'gameplay' ? 'Jugabilidad' : english;
    const rows = buildModel(
      [mod({
        id: 'acme.widgets',
        settings: [{ type: 'key', key: 'openMenu' }],
        values: { openMenu: 'F7' },
      })],
      [gameAction('QuickSaveHandler', 'Quicksave', 'F5')],
      translate,
    );
    expect(rows[0]!.contextLabel).toBe('Jugabilidad');
    expect(rows[1]!.contextLabel).toBe('MainGameplay');
  });
});

describe('buildModel', () => {
  it('builds one row per bound key setting', () => {
    const rows = buildModel(
      [
        mod({
          id: 'osfui',
          title: 'OSF UI',
          settings: [{ key: 'toggleKey', label: 'Open / close key', type: 'key' }],
          values: { toggleKey: 'F10' },
        }),
      ],
      [],
    );
    expect(rows).toEqual([
      {
        kind: 'mod',
        mod: 'osfui',
        key: 'toggleKey',
        label: 'Open / close key',
        owner: 'OSF UI',
        name: 'F10',
        keyLabel: 'F10',
        contextId: 'gameplay',
        contextLabel: 'Gameplay',
        blocksGameplay: false,
        gameplayModes: null,
        chord: ['F10'],
        unbound: false,
        vanillaWarnings: true,
        rowId: 'mod:osfui:toggleKey',
      },
    ]);
  });

  it('produces NO row for an unbound key setting', () => {
    const rows = buildModel(
      [
        mod({
          id: 'm',
          settings: [
            { key: 'bound', type: 'key' },
            { key: 'unbound', type: 'key' },
            { key: 'missing', type: 'key' },
          ],
          // "" is the allowUnbound state: no row, so an unbound key can never
          // conflict with anything.
          values: { bound: 'F5', unbound: '' },
        }),
      ],
      [],
    );
    expect(rows.map((r) => r.key)).toEqual(['bound']);
  });

  it('ignores non-key settings, keyless items and non-string values', () => {
    const rows = buildModel(
      [
        mod({
          id: 'm',
          settings: [
            { key: 'volume', type: 'number' },
            { key: 'on', type: 'bool' },
            { type: 'note', text: 'hi' },
            { key: 5, type: 'key' },
            { key: 'numeric', type: 'key' },
            { key: 'real', type: 'key' },
          ],
          values: { volume: 1, on: true, numeric: 42, real: 'F1' },
        }),
      ],
      [],
    );
    expect(rows.map((r) => r.key)).toEqual(['real']);
  });

  it('canonicalises the stored value', () => {
    const rows = buildModel(
      [
        mod({
          id: 'm',
          settings: [
            { key: 'a', type: 'key' },
            { key: 'b', type: 'key' },
            { key: 'c', type: 'key' },
          ],
          values: { a: 'Tilde', b: 'return', c: 'q' },
        }),
      ],
      [],
    );
    expect(rows.map((r) => r.name)).toEqual(['Grave', 'Enter', 'Q']);
  });

  it('degrades an absent label to the setting key, and an absent title to the mod id', () => {
    const rows = buildModel(
      [mod({ id: 'modid', settings: [{ key: 'k', type: 'key' }], values: { k: 'F1' } })],
      [],
    );
    expect(rows[0]?.label).toBe('k');
    expect(rows[0]?.owner).toBe('modid');
  });

  it('carries a resolved input context onto the row', () => {
    const rows = buildModel(
      [
        mod({
          id: 'm',
          settings: [{ key: 'k', type: 'key', inputContext: 'menu' }],
          values: { k: 'F1' },
          inputContexts: [{ id: 'menu', label: 'Menu', blocksGameplay: true }],
        }),
      ],
      [],
    );
    expect(rows[0]).toMatchObject({
      contextId: 'menu',
      contextLabel: 'Menu',
      blocksGameplay: true,
    });
  });

  it('builds live game rows from osfui/keybindings', () => {
    const actions: KeybindingsData['actions'] = [
      gameAction('QuickSave', 'Quicksave', 'F5'),
      gameAction('Console', 'Console', 'Tilde'),
    ];
    const rows = buildModel([], actions);
    expect(rows).toEqual([
      {
        kind: 'game',
        key: 'QuickSave',
        label: 'Quicksave',
        owner: 'Starfield',
        name: 'F5',
        keyLabel: 'F5',
        contextId: 'MainGameplay',
        contextLabel: 'MainGameplay',
        contextNumericId: 0,
        category: 'Gameplay',
        classification: 'core',
        gameplayModes: ['onFoot', 'ship', 'vehicle', 'zeroG'],
        blocksGameplay: false,
        chord: ['F5'],
        unbound: false,
        vanillaWarnings: true,
        slot: 'main',
        rowId: 'game:0:QuickSave:main:0',
      },
      {
        kind: 'game',
        key: 'Console',
        label: 'Console',
        owner: 'Starfield',
        // The vanilla name is alias-folded too, so it groups with a mod that
        // stored "Grave".
        name: 'Grave',
        keyLabel: 'Grave',
        contextId: 'MainGameplay',
        contextLabel: 'MainGameplay',
        contextNumericId: 0,
        category: 'Gameplay',
        classification: 'core',
        gameplayModes: ['onFoot', 'ship', 'vehicle', 'zeroG'],
        blocksGameplay: false,
        chord: ['Grave'],
        unbound: false,
        vanillaWarnings: true,
        slot: 'main',
        rowId: 'game:0:Console:main:0',
      },
    ]);
  });

  it('keeps live main/alternate, chord, and unbound vanilla rows in the list model', () => {
    const rows = buildModel([], [{
      event: 'UseThing', label: 'Use thing', category: 'Ship',
      context: { id: 0x21, name: 'ShipHUD', order: 1 }, classification: 'core',
      modes: { definite: ['ship'], possible: [] }, sortIndex: 4, required: false,
      bindings: [
        { slot: 'main', key: 'F5', chord: ['F5'], unbound: false },
        { slot: 'alternate', key: 'K', chord: ['LCtrl', 'K'], unbound: false },
        { slot: 'alternate', key: null, chord: [], unbound: true },
      ],
    }]);
    expect(rows).toHaveLength(3);
    expect(rows[0]).toMatchObject({ name: 'F5', slot: 'main', contextId: 'ShipHUD', gameplayModes: ['ship'] });
    expect(rows[1]).toMatchObject({ name: '', slot: 'alternate', chord: ['LCtrl', 'K'], keyLabel: 'LCtrl + K' });
    expect(rows[2]).toMatchObject({ name: '', unbound: true, keyLabel: 'Unbound' });
    expect(holdersOf(rows, 'F5')).toHaveLength(1);
    expect(holdersOf(rows, '')).toEqual([]);
  });

  it('routes the game owner through the translator and preserves engine context names', () => {
    const rows = buildModel([], [gameAction('E', 'X', 'F1')], (address) =>
      address === 'gameOwner' ? 'Sternenfeld' : 'Spielablauf',
    );
    expect(rows[0]?.owner).toBe('Sternenfeld');
    expect(rows[0]?.contextLabel).toBe('MainGameplay');
  });

  it('emits mod rows before game rows', () => {
    const rows = buildModel(
      [mod({ id: 'm', settings: [{ key: 'k', type: 'key' }], values: { k: 'F1' } })],
      [gameAction('E', 'X', 'F2')],
    );
    expect(rows.map((r) => r.kind)).toEqual(['mod', 'game']);
  });

  it('degrades falsy (not just nullish) groups/settings/values to empty', () => {
    // A hand-edited or hostile manifest carrying `groups: 0` must degrade to no
    // rows, not throw out of the for-of and kill the whole render.
    const junk = [
      { id: 'a', schema: { groups: 0 } },
      { id: 'b', schema: { groups: [{ settings: 0 }] } },
      { id: 'c', schema: { groups: [{ settings: [{ key: 'k', type: 'key' }] }] }, values: 0 },
    ] as unknown as ModEntry[];
    expect(buildModel(junk, [])).toEqual([]);
  });

  it('skips null entries rather than throwing (documented divergence)', () => {
    // Native never sends a null entry; skipping one beats throwing out of the
    // render.
    const rows = buildModel(
      [null, mod({ id: 'm', settings: [{ key: 'k', type: 'key' }], values: { k: 'F1' } })] as
        unknown as ModEntry[],
      [null, gameAction('E', 'X', 'F2')] as unknown as KeybindingsData['actions'],
    );
    expect(rows.map((r) => r.name)).toEqual(['F1', 'F2']);
  });

  it('tolerates missing mods/game actions entirely', () => {
    expect(buildModel(undefined, undefined)).toEqual([]);
    expect(buildModel(null, null)).toEqual([]);
    expect(buildModel([mod({ id: 'm', settings: [] })], [])).toEqual([]);
  });
});
