// Who holds a key, and is that a problem?
//
// Collisions are derived here by grouping on the canonical key name rather than
// consuming the per-setting `conflicts` array native pushes. Both agree because
// canonicalName() folds the same aliases native's VK resolution does, and
// deriving locally means a repaint after an optimistic rebind needs no round trip.

import type { BindingRow } from './model';

/** Every row bound to `name`, in model order. Exact string match, no folding — rows are already canonical. */
export function holdersOf(rows: readonly BindingRow[], name: string): BindingRow[] {
  return rows.filter((b) => b.name === name);
}

/**
 * Is this pair of holders an expected share rather than a conflict?
 *
 * True in exactly one arrangement: one side is a mod row, the other a game row,
 * and the mod side declares an input context with blocksGameplay — i.e. while
 * that context is active the game does not see the key, so reusing a vanilla
 * binding is intentional.
 *
 * The asymmetry is intended:
 *   * mod-vs-mod — always a conflict, even if both block gameplay. Blocking
 *     gameplay says nothing about another mod's dispatch; both still fire.
 *   * game-vs-game — always a conflict. `mod` is null so blocksGameplay is
 *     never read. (Vanilla rows are hardcoded blocksGameplay:false anyway.)
 *   * mod-vs-game with the game side flagged — still a conflict; the flag is
 *     read off the mod-kind side. Unreachable with rows from buildModel.
 */
export function pairIsShared(a: BindingRow, b: BindingRow): boolean {
  const mod =
    a.kind === 'mod' && b.kind === 'game' ? a : b.kind === 'mod' && a.kind === 'game' ? b : null;
  return !!(mod && mod.blocksGameplay);
}

/** Per-key badge state. Both flags can be true at once — see keyState(). */
export interface ConflictState {
  conflict: boolean;
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
  let shared = false;
  for (let i = 0; i < holders.length; ++i) {
    for (let j = i + 1; j < holders.length; ++j) {
      const a = holders[i];
      const b = holders[j];
      if (!a || !b) continue; // unreachable; satisfies noUncheckedIndexedAccess
      if (pairIsShared(a, b)) shared = true;
      else conflict = true;
    }
  }
  return { conflict, shared };
}

/**
 * State for one binding: how it relates to the other holders of its key.
 *
 * Self is excluded by identity, not by value, because two rows can be
 * field-for-field identical (same mod registering a label twice, duplicated
 * vanillaKeys entry) and those must report as conflicting. Callers must pass a
 * row from the same array they are querying; a structurally-equal clone would
 * compare against itself and self-report a conflict.
 */
export function holderState(rows: readonly BindingRow[], binding: BindingRow): ConflictState {
  let conflict = false;
  let shared = false;
  for (const other of holdersOf(rows, binding.name)) {
    if (other === binding) continue;
    if (pairIsShared(binding, other)) shared = true;
    else conflict = true;
  }
  return { conflict, shared };
}
