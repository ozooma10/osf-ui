// conflicts.ts — who holds a key, and is that a problem?
//
// Extracted from src/views/osfui/keybinds/main.legacy.js:143-175.
//
// The board derives collisions itself, by grouping on the CANONICAL key name,
// rather than consuming the per-setting `conflicts` array native pushes. Both
// agree because canonicalName() folds the aliases native's VK resolution would
// (see canonical.ts), and deriving locally means a repaint after an optimistic
// rebind needs no round trip.

import type { BindingRow } from './model';

/** Every row bound to `name`, in model order. Exact string match, no folding — rows are already canonical. */
export function holdersOf(rows: readonly BindingRow[], name: string): BindingRow[] {
  return rows.filter((b) => b.name === name);
}

/**
 * Is this PAIR of holders an expected share rather than a conflict?
 *
 * True in exactly one arrangement: one side is a mod row, the other is a game
 * row, AND THE MOD SIDE declares an input context with blocksGameplay. The
 * assertion means "while my context is active the game does not see this key",
 * so reusing a vanilla binding is intentional, not a collision.
 *
 * ASYMMETRY, and it is deliberate (legacy 147-151):
 *   * mod-vs-mod  — always a conflict, even if BOTH block gameplay. Blocking
 *     gameplay says nothing about another mod's dispatch; two mods that both
 *     claim the key still both fire.
 *   * game-vs-game — always a conflict. `mod` resolves to null, so the
 *     blocksGameplay check never runs. (Vanilla rows are hardcoded
 *     blocksGameplay:false anyway — see buildModel.)
 *   * mod-vs-game where the GAME side is somehow flagged — still a conflict,
 *     because the flag is read off `mod`, the mod-kind side. Unreachable with
 *     rows from buildModel, but the rule is what the code says.
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
 * State for a whole key: walk every unordered pair of its holders and classify
 * each one independently.
 *
 * BOTH FLAGS CAN BE TRUE simultaneously, and that is not a bug — three holders
 * produce three pairs, and e.g. {blocking mod, plain mod, game} yields one
 * shared pair (blocking mod vs game) and two conflicting ones. The detail
 * panel renders both badges in that case; the board resolves the ambiguity in
 * CSS instead, painting `is-shared` only when `shared && !conflict` so the
 * louder conflict styling wins (legacy 253-254).
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
 * State for ONE binding: how it relates to the other holders of its key.
 *
 * Self is excluded BY IDENTITY (`other === binding`), not by value — legacy
 * line 170. That matters: two rows can be field-for-field identical (the same
 * mod registering the same label twice, or a duplicated vanillaKeys entry),
 * and identity comparison correctly reports those as conflicting with each
 * other. Callers must therefore pass a row that came out of the SAME array
 * they are querying; a structurally-equal clone would compare against itself
 * and self-report a conflict.
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
