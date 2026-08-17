import { describe, it, expect } from 'vitest';
import type { ModRecord, RailModel, RailNode, ViewRecord } from '@lib/settings/rail';
import {
  FRAMEWORK_ID,
  findEntry,
  railEntries,
  railNodes,
  titleOf,
} from '@lib/settings/rail';

const framework: ModRecord = { id: FRAMEWORK_ID, title: 'OSF UI' };
const zeta: ModRecord = { id: 'acme.zeta', title: 'Zeta Tools' };
const alpha: ModRecord = { id: 'acme.alpha', title: 'alpha works' };

const view = (o: Partial<ViewRecord> & { id: string }): ViewRecord => o;

const kb = view({ id: 'osfui/keybinds', title: 'Keybindings', mod: FRAMEWORK_ID, kind: 'menu' });
const zetaHud = view({ id: 'acme.zeta/hud', title: 'Zeta HUD', mod: 'acme.zeta', kind: 'hud' });
const orphanHud = view({ id: 'solo/hud', title: 'Solo HUD', mod: 'solo.mod', kind: 'hud' });
const orphanMenu = view({ id: 'solo/menu', title: 'Solo Terminal', mod: 'solo.mod', kind: 'menu' });
const standalone = view({ id: 'lone/panel', title: 'Lone Panel', kind: 'menu' });

const ids = (nodes: RailNode[]): string[] =>
  nodes.map((n) => (n.kind === 'entry' ? n.entry.id : n.kind));

describe('titleOf', () => {
  it('prefers title, then schema.title, then the id', () => {
    expect(titleOf({ id: 'x', title: 'T', schema: { title: 'S' } })).toBe('T');
    expect(titleOf({ id: 'x', schema: { title: 'S' } })).toBe('S');
    expect(titleOf({ id: 'x' })).toBe('x');
    // Empty strings are falsy, so they fall through rather than blanking the rail.
    expect(titleOf({ id: 'x', title: '' })).toBe('x');
  });
});

describe('railEntries — union of settings mods and catalog views', () => {
  it('gives every settings mod an entry with its attached views', () => {
    const entries = railEntries([framework, zeta], [kb, zetaHud]);
    expect(entries.map((e) => e.id)).toEqual([FRAMEWORK_ID, 'acme.zeta']);
    expect(entries[0]?.views).toEqual([kb]);
    expect(entries[1]?.views).toEqual([zetaHud]);
  });

  it('gives an orphan view group a "view:" entry keyed on its manifest mod', () => {
    const entries = railEntries([], [orphanHud, orphanMenu]);
    expect(entries.map((e) => e.id)).toEqual(['view:solo.mod']);
    expect(entries[0]?.mod).toBeNull();
    expect(entries[0]?.views).toEqual([orphanHud, orphanMenu]);
  });

  it('titles an orphan group from its first kind:"menu" view, not the first view', () => {
    // orphanHud is first in the list, but the menu names the group better.
    expect(railEntries([], [orphanHud, orphanMenu])[0]?.title).toBe('Solo Terminal');
  });

  it('falls back to the first view of any kind when there is no menu', () => {
    expect(railEntries([], [orphanHud])[0]?.title).toBe('Solo HUD');
  });

  it('keys a view with NO manifest mod on its own view id', () => {
    const entries = railEntries([], [standalone]);
    expect(entries[0]?.id).toBe('view:lone/panel');
    expect(entries[0]?.title).toBe('Lone Panel');
  });

  it('falls back to the grouping key when the lead view has no title', () => {
    const entries = railEntries([], [view({ id: 'x/y', mod: 'ghost.mod' })]);
    expect(entries[0]?.title).toBe('ghost.mod');
  });

  it('does NOT orphan a view whose mod IS loaded', () => {
    expect(railEntries([zeta], [zetaHud]).map((e) => e.id)).toEqual(['acme.zeta']);
  });
});

describe('findEntry', () => {
  it('finds by id and returns undefined otherwise', () => {
    expect(findEntry([zeta], [], 'acme.zeta')?.title).toBe('Zeta Tools');
    expect(findEntry([zeta], [], 'nope')).toBeUndefined();
    expect(findEntry([zeta], [], null)).toBeUndefined();
    expect(findEntry([], [orphanMenu], 'view:solo.mod')?.mod).toBeNull();
  });
});

describe('railNodes — paint order', () => {
  const model: RailModel = { mods: [zeta, framework, alpha], views: [] };

  it('is Home, framework, "Mods" header, then sorted mods', () => {
	expect(ids(railNodes(model, ''))).toEqual([
	  'home',
      FRAMEWORK_ID,
      'section',
      // localeCompare sensitivity "base": "alpha works" sorts before "Zeta
      // Tools" despite the case difference.
      'acme.alpha',
      'acme.zeta',
    ]);
  });

  it('sorts case- and accent-insensitively, not by ASCII', () => {
    const mixed: RailModel = {
      mods: [{ id: 'b', title: 'beta' }, { id: 'a', title: 'Alpha' }],
      views: [],
    };
	expect(ids(railNodes(mixed, ''))).toEqual(['home', 'section', 'a', 'b']);
  });

  it('DROPS Home while a filter is active', () => {
	expect(ids(railNodes(model, 'zeta'))).toEqual(['section', 'acme.zeta']);
  });

  it('shows the filtered-empty state when the filter matches nothing', () => {
	expect(ids(railNodes({ mods: [], views: [] }, 'zzz'))).toEqual(['section', 'empty']);
  });

  it('always emits the "Mods" header, even with an empty list', () => {
    const nodes = railNodes({ mods: [], views: [] }, '');
    expect(nodes.some((n) => n.kind === 'section')).toBe(true);
  });

  it('distinguishes the two empty states', () => {
    const filtered = railNodes({ mods: [zeta], views: [] }, 'zzz');
    const none = railNodes({ mods: [], views: [] }, '');
    expect(filtered.find((n) => n.kind === 'empty')).toEqual({ kind: 'empty', reason: 'filtered' });
    expect(none.find((n) => n.kind === 'empty')).toEqual({ kind: 'empty', reason: 'none' });
  });

  it('hides the framework entry when it does not match the filter', () => {
	expect(ids(railNodes(model, 'alpha'))).toEqual(['section', 'acme.alpha']);
  });
});
