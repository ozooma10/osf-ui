import { describe, expect, it } from 'vitest';
import type { GameBindingRow, ModBindingRow } from '@lib/keybinds/model';
import { matchesBindingFilter, prioritizeBindingsForFilter } from '@lib/keybinds/filter';

const game = (overrides: Partial<GameBindingRow> = {}): GameBindingRow => ({
  kind: 'game', key: 'Event', label: 'Action', owner: 'Starfield', name: 'F5', keyLabel: 'F5',
  engineInputContextName: 'MainGameplay', engineInputContextLabel: 'MainGameplay', engineInputContextId: 0,
  classification: 'core', gameplayModes: ['onFoot', 'ship'],
  chord: ['F5'], unbound: false, rowId: 'g', ...overrides,
});

const mod = (overrides: Partial<ModBindingRow> = {}): ModBindingRow => ({
  kind: 'mod', mod: 'a.mod', key: 'open', label: 'Open', owner: 'A', name: 'F5', keyLabel: 'F5',
  hotkeyContextId: 'ship', hotkeyContextLabel: 'Ship', gameplayModes: ['ship'], blocksGameplay: false,
  chord: ['F5'], unbound: false, rowId: 'm', ...overrides,
});

describe('keybind list filters', () => {
  const active = { available: true, revision: 1, mode: 'ship' as const, contexts: [{ id: 0, name: 'MainGameplay' }] };

  it('filters active engine input contexts and semantic mod modes', () => {
    expect(matchesBindingFilter(game(), 'active', active)).toBe(true);
    expect(matchesBindingFilter(game({ engineInputContextId: 0x18 }), 'active', active)).toBe(false);
    expect(matchesBindingFilter(mod(), 'active', active)).toBe(true);
    expect(matchesBindingFilter(mod({ gameplayModes: ['onFoot'] }), 'active', active)).toBe(false);
  });

  it('separates gameplay, menu, other, mode, category, and unbound rows', () => {
    expect(matchesBindingFilter(game(), 'gameplay', active)).toBe(true);
    expect(matchesBindingFilter(game({ classification: 'menu' }), 'menu', active)).toBe(true);
    expect(matchesBindingFilter(game({ classification: 'unknown' }), 'other', active)).toBe(true);
    expect(matchesBindingFilter(game(), 'ship', active)).toBe(true);
    expect(matchesBindingFilter(game({ gameplayModes: ['onFoot'] }), 'vehicle', active)).toBe(false);
    expect(matchesBindingFilter(game({ category: 'Ship' }), 'category:Ship', active)).toBe(true);
    expect(matchesBindingFilter(game({ name: '', chord: [], unbound: true }), 'unbound', active)).toBe(true);
  });

  it('prioritizes the selected layer without hiding or reordering either group', () => {
    const menu = game({ rowId: 'menu', classification: 'menu', category: 'Menu' });
    const shipA = game({ rowId: 'ship-a', category: 'Ship' });
    const other = mod({ rowId: 'mod' });
    const shipB = game({ rowId: 'ship-b', category: 'Ship' });

    expect(prioritizeBindingsForFilter([menu, shipA, other, shipB], 'category:Ship', active))
      .toEqual([shipA, shipB, menu, other]);
  });
});
