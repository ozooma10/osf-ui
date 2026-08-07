// Who holds a key, and is that a problem?
//
// Collisions are derived here by grouping on the canonical key name rather than
// consuming the per-setting `conflicts` array native pushes. Both agree because
// canonicalName() folds the same aliases native's VK resolution does, and
// deriving locally means a repaint after an optimistic rebind needs no round trip.

import type { BindingRow, GameBindingRow, ModBindingRow } from './model';

/** Every row bound to `name`, in model order. Exact string match, no folding — rows are already canonical. */
export function holdersOf(rows: readonly BindingRow[], name: string): BindingRow[] {
  return name ? rows.filter((b) => b.name === name) : [];
}

type PairState = 'conflict' | 'possible' | 'shared' | 'neutral';

function modesOverlap(a: BindingRow, b: BindingRow): boolean {
  if (!a.gameplayModes || !b.gameplayModes) return true;
  return a.gameplayModes.some((mode) => b.gameplayModes?.includes(mode));
}

function classifyModGame(mod: ModBindingRow, game: GameBindingRow): PairState {
  if (game.gameBindingWarnings === false) return 'neutral';
  if (mod.blocksGameplay) return 'shared';
  if (!modesOverlap(mod, game)) return 'shared';
  if (!game.classification || game.classification === 'core') return 'conflict';
  if (game.classification === 'special') return 'possible';
  return 'neutral';
}

export function classifyPair(a: BindingRow, b: BindingRow): PairState {
  if (!a.name || a.name !== b.name) return 'neutral';
  if (a.kind === 'game' && b.kind === 'game') return 'neutral';
  if (a.kind === 'mod' && b.kind === 'mod') return modesOverlap(a, b) ? 'conflict' : 'shared';
  if (a.kind === 'mod' && b.kind === 'game') return classifyModGame(a, b);
  if (a.kind === 'game' && b.kind === 'mod') return classifyModGame(b, a);
  return 'neutral';
}

/** True when matching keys are an intentional or proven-disjoint share. */
export function pairIsShared(a: BindingRow, b: BindingRow): boolean {
  return classifyPair(a, b) === 'shared';
}

/** Per-key badge state. Both flags can be true at once — see keyState(). */
export interface ConflictState {
  conflict: boolean;
  possible?: boolean;
  shared: boolean;
}

/**
 * State for a whole key: classify every unordered pair of its holders.
 *
 * Both flags can be true at once — three holders make three pairs, so
 * {blocking mod, plain mod, game} yields one shared pair and two conflicting
 * ones. The detail panel renders both badges; the board paints `is-shared` only
 * when `shared && !conflict` so the louder conflict styling wins.
 *
 * A key with 0 or 1 holders has no pairs, so both flags are false.
 */
export function keyState(rows: readonly BindingRow[], name: string): ConflictState {
  const holders = holdersOf(rows, name);
  let conflict = false;
  let possible = false;
  let shared = false;
  for (let i = 0; i < holders.length; ++i) {
    for (let j = i + 1; j < holders.length; ++j) {
      const a = holders[i];
      const b = holders[j];
      if (!a || !b) continue; // unreachable; satisfies noUncheckedIndexedAccess
      const state = classifyPair(a, b);
      if (state === 'shared') shared = true;
      else if (state === 'possible') possible = true;
      else if (state === 'conflict') conflict = true;
    }
  }
  return possible ? { conflict, possible: true, shared } : { conflict, shared };
}

/**
 * State for one binding: how it relates to the other holders of its key.
 *
 * Self is excluded by identity, not by value, because two rows can be
 * field-for-field identical (same mod registering a label twice, duplicated
 * game-binding catalog entry) and those must report as conflicting. Callers must pass a
 * row from the same array they are querying; a structurally-equal clone would
 * compare against itself and self-report a conflict.
 */
export function holderState(rows: readonly BindingRow[], binding: BindingRow): ConflictState {
  let conflict = false;
  let possible = false;
  let shared = false;
  for (const other of holdersOf(rows, binding.name)) {
    if (other === binding) continue;
    const state = classifyPair(binding, other);
    if (state === 'shared') shared = true;
    else if (state === 'possible') possible = true;
    else if (state === 'conflict') conflict = true;
  }
  return possible ? { conflict, possible: true, shared } : { conflict, shared };
}
