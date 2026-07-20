import { describe, it, expect } from 'vitest';
import { holdersOf, pairIsShared, keyState, holderState } from '@lib/keybinds/conflicts';
import type { BindingRow } from '@lib/keybinds/model';

function modRow(name: string, opts?: { owner?: string; blocks?: boolean }): BindingRow {
  return {
    kind: 'mod',
    mod: opts?.owner ?? 'm',
    key: 'k',
    label: 'l',
    owner: opts?.owner ?? 'm',
    name,
    contextId: opts?.blocks ? 'menu' : 'gameplay',
    contextLabel: opts?.blocks ? 'Menu' : 'Gameplay',
    blocksGameplay: opts?.blocks ?? false,
  };
}

function gameRow(name: string, opts?: { blocks?: boolean }): BindingRow {
  return {
    kind: 'game',
    key: 'Event',
    label: 'l',
    owner: 'Starfield',
    name,
    contextId: 'gameplay',
    contextLabel: 'Gameplay',
    blocksGameplay: opts?.blocks ?? false,
  };
}

describe('holdersOf', () => {
  it('matches the canonical name exactly and preserves model order', () => {
    const a = modRow('F5');
    const b = gameRow('F5');
    const c = modRow('F9');
    expect(holdersOf([a, c, b], 'F5')).toEqual([a, b]);
    expect(holdersOf([a, c, b], 'F9')).toEqual([c]);
    expect(holdersOf([a, c, b], 'F1')).toEqual([]);
    // No folding here — rows are canonical already.
    expect(holdersOf([modRow('Grave')], 'Tilde')).toEqual([]);
  });
});

describe('pairIsShared', () => {
  it('is true only when the MOD side of a mod/game pair blocks gameplay', () => {
    const blocking = modRow('F5', { blocks: true });
    const game = gameRow('F5');
    expect(pairIsShared(blocking, game)).toBe(true);
    // Symmetric in argument order — the mod side is found either way.
    expect(pairIsShared(game, blocking)).toBe(true);
  });

  it('is false for a mod/game pair when the mod does not block gameplay', () => {
    expect(pairIsShared(modRow('F5'), gameRow('F5'))).toBe(false);
  });

  it('ASYMMETRY: mod-vs-mod always conflicts, even when BOTH block gameplay', () => {
    const a = modRow('F5', { owner: 'a', blocks: true });
    const b = modRow('F5', { owner: 'b', blocks: true });
    // Blocking gameplay says nothing about another mod's dispatch: both fire.
    expect(pairIsShared(a, b)).toBe(false);
  });

  it('ASYMMETRY: game-vs-game always conflicts, even with the flag set', () => {
    // Unreachable via buildModel (vanilla rows hardcode false), but `mod`
    // resolves to null so the flag is never read.
    expect(pairIsShared(gameRow('F5', { blocks: true }), gameRow('F5', { blocks: true }))).toBe(
      false,
    );
  });

  it('ASYMMETRY: the flag is read off the MOD side, never the game side', () => {
    const plainMod = modRow('F5');
    const flaggedGame = gameRow('F5', { blocks: true });
    expect(pairIsShared(plainMod, flaggedGame)).toBe(false);
    expect(pairIsShared(flaggedGame, plainMod)).toBe(false);
  });
});

describe('keyState', () => {
  it('reports neither flag for 0 or 1 holders (no pairs exist)', () => {
    expect(keyState([], 'F5')).toEqual({ conflict: false, shared: false });
    expect(keyState([modRow('F5')], 'F5')).toEqual({ conflict: false, shared: false });
    expect(keyState([modRow('F5', { blocks: true })], 'F5')).toEqual({
      conflict: false,
      shared: false,
    });
  });

  it('reports shared for a blocking mod over a vanilla binding', () => {
    const rows = [modRow('F5', { blocks: true }), gameRow('F5')];
    expect(keyState(rows, 'F5')).toEqual({ conflict: false, shared: true });
  });

  it('reports conflict for two mods on one key', () => {
    const rows = [modRow('F5', { owner: 'a' }), modRow('F5', { owner: 'b' })];
    expect(keyState(rows, 'F5')).toEqual({ conflict: true, shared: false });
  });

  it('returns BOTH flags true when a key has a shared pair and a conflicting pair', () => {
    // {blocking mod, plain mod, game} = 3 pairs:
    //   blocking x plain -> conflict (mod vs mod)
    //   blocking x game  -> shared
    //   plain    x game  -> conflict
    const rows = [
      modRow('F5', { owner: 'blocker', blocks: true }),
      modRow('F5', { owner: 'plain' }),
      gameRow('F5'),
    ];
    expect(keyState(rows, 'F5')).toEqual({ conflict: true, shared: true });
  });

  it('ignores holders of other keys', () => {
    const rows = [modRow('F5', { owner: 'a' }), modRow('F9', { owner: 'b' })];
    expect(keyState(rows, 'F5')).toEqual({ conflict: false, shared: false });
  });
});

describe('holderState', () => {
  it('excludes self, so a lone holder is clean', () => {
    const a = modRow('F5');
    expect(holderState([a], a)).toEqual({ conflict: false, shared: false });
  });

  it('excludes self BY IDENTITY, not by value', () => {
    // Two structurally identical rows (a mod registering the same binding
    // twice, or a duplicated vanillaKeys entry) do conflict with each other.
    const a = modRow('F5');
    const clone = modRow('F5');
    expect(a).toEqual(clone); // same value...
    expect(holderState([a, clone], a)).toEqual({ conflict: true, shared: false });

    // ...and a row that is not in the array compares against its twin, so it
    // self-reports a conflict. Callers must pass a row from the same array.
    const outsider = modRow('F5');
    expect(holderState([a], outsider)).toEqual({ conflict: true, shared: false });
  });

  it('classifies each other holder independently, and can report both flags', () => {
    const blocker = modRow('F5', { owner: 'blocker', blocks: true });
    const plain = modRow('F5', { owner: 'plain' });
    const game = gameRow('F5');
    const rows = [blocker, plain, game];

    // The blocking mod shares with the game but conflicts with the other mod.
    expect(holderState(rows, blocker)).toEqual({ conflict: true, shared: true });
    // The plain mod conflicts with both.
    expect(holderState(rows, plain)).toEqual({ conflict: true, shared: false });
    // The game row shares with the blocker and conflicts with the plain mod.
    expect(holderState(rows, game)).toEqual({ conflict: true, shared: true });
  });

  it('reports shared-only for a blocking mod against just the game', () => {
    const blocker = modRow('F5', { blocks: true });
    const game = gameRow('F5');
    expect(holderState([blocker, game], blocker)).toEqual({ conflict: false, shared: true });
    expect(holderState([blocker, game], game)).toEqual({ conflict: false, shared: true });
  });
});
