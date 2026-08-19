
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
