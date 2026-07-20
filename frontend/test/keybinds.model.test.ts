import { describe, it, expect } from 'vitest';
import { buildModel, vanillaLabel, inputContextFor } from '@lib/keybinds/model';
import type { ModEntry, VanillaKey } from '@lib/keybinds/model';

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

describe('vanillaLabel', () => {
  it('strips the "Game (Label)" wrapper', () => {
    expect(vanillaLabel('Starfield (Quicksave)')).toBe('Quicksave');
    expect(vanillaLabel('Starfield (Toggle POV)')).toBe('Toggle POV');
  });

  it('keeps the innermost content when the label itself has parens', () => {
    // `(.+)` is greedy and anchored on the trailing ")", so nested parens on
    // the right are kept whole.
    expect(vanillaLabel('Starfield (Scanner (flashlight))')).toBe('Scanner (flashlight)');
  });

  it('passes non-matching titles through unchanged', () => {
    expect(vanillaLabel('Quicksave')).toBe('Quicksave');
    // No space before the paren.
    expect(vanillaLabel('Starfield(Quicksave)')).toBe('Starfield(Quicksave)');
    // Empty parens: `(.+)` needs at least one char.
    expect(vanillaLabel('Starfield ()')).toBe('Starfield ()');
    // Trailing text after the close paren.
    expect(vanillaLabel('Starfield (Quicksave) v2')).toBe('Starfield (Quicksave) v2');
    // `[^(]+` forbids an open paren in the prefix.
    expect(vanillaLabel('(Starfield) (Quicksave)')).toBe('(Starfield) (Quicksave)');
    // Nothing before the space-paren.
    expect(vanillaLabel(' (Quicksave)')).toBe(' (Quicksave)');
  });

  it('coerces falsy input to ""', () => {
    expect(vanillaLabel(undefined)).toBe('');
    expect(vanillaLabel(null)).toBe('');
    expect(vanillaLabel('')).toBe('');
  });
});

describe('inputContextFor', () => {
  const schema = {
    inputContexts: [
      { id: 'menu', label: 'Menu', blocksGameplay: true },
      { id: 'nolabel' },
      { id: 'empty', label: '' },
      { id: 'truthy', blocksGameplay: 1 },
    ],
  } as never;

  it('falls back to gameplay for an absent, empty or literal-gameplay ref', () => {
    const fallback = { id: 'gameplay', label: 'Gameplay', blocksGameplay: false };
    expect(inputContextFor(schema, undefined)).toEqual(fallback);
    expect(inputContextFor(schema, '')).toEqual(fallback);
    expect(inputContextFor(schema, 'gameplay')).toEqual(fallback);
  });

  it('falls back for an unknown ref, or one that fails the id grammar', () => {
    expect(inputContextFor(schema, 'nosuch').id).toBe('gameplay');
    expect(inputContextFor(schema, '-leading-dash').id).toBe('gameplay');
    expect(inputContextFor(schema, 'has space').id).toBe('gameplay');
    expect(inputContextFor(undefined, 'menu').id).toBe('gameplay');
  });

  it('resolves a declared context', () => {
    expect(inputContextFor(schema, 'menu')).toEqual({
      id: 'menu',
      label: 'Menu',
      blocksGameplay: true,
    });
  });

  it('defaults a missing or empty label to the id', () => {
    expect(inputContextFor(schema, 'nolabel').label).toBe('nolabel');
    expect(inputContextFor(schema, 'empty').label).toBe('empty');
  });

  it('requires blocksGameplay === true exactly (a truthy non-boolean does not count)', () => {
    expect(inputContextFor(schema, 'truthy').blocksGameplay).toBe(false);
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
        contextId: 'gameplay',
        contextLabel: 'Gameplay',
        blocksGameplay: false,
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

  it('makes vanilla rows gameplay/non-blocking, always', () => {
    const vanilla: VanillaKey[] = [
      { event: 'QuickSave', title: 'Starfield (Quicksave)', name: 'F5' },
      { event: 'Console', title: 'Starfield (Console)', name: 'Tilde' },
    ];
    const rows = buildModel([], vanilla);
    expect(rows).toEqual([
      {
        kind: 'game',
        key: 'QuickSave',
        label: 'Quicksave',
        owner: 'Starfield',
        name: 'F5',
        contextId: 'gameplay',
        contextLabel: 'Gameplay',
        blocksGameplay: false,
      },
      {
        kind: 'game',
        key: 'Console',
        label: 'Console',
        owner: 'Starfield',
        // The vanilla name is alias-folded too, so it groups with a mod that
        // stored "Grave".
        name: 'Grave',
        contextId: 'gameplay',
        contextLabel: 'Gameplay',
        blocksGameplay: false,
      },
    ]);
  });

  it('routes the game owner and context label through the injected translator', () => {
    const rows = buildModel([], [{ event: 'E', title: 'Starfield (X)', name: 'F1' }], (address) =>
      address === 'gameOwner' ? 'Sternenfeld' : 'Spielablauf',
    );
    expect(rows[0]?.owner).toBe('Sternenfeld');
    expect(rows[0]?.contextLabel).toBe('Spielablauf');
  });

  it('emits mod rows before game rows', () => {
    const rows = buildModel(
      [mod({ id: 'm', settings: [{ key: 'k', type: 'key' }], values: { k: 'F1' } })],
      [{ event: 'E', title: 'Starfield (X)', name: 'F2' }],
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
      [null, { event: 'E', title: 'Starfield (X)', name: 'F2' }] as unknown as VanillaKey[],
    );
    expect(rows.map((r) => r.name)).toEqual(['F1', 'F2']);
  });

  it('tolerates missing mods/vanillaKeys entirely', () => {
    expect(buildModel(undefined, undefined)).toEqual([]);
    expect(buildModel(null, null)).toEqual([]);
    expect(buildModel([mod({ id: 'm', settings: [] })], [])).toEqual([]);
  });
});
