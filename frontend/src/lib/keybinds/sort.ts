// Ordering for the "All bindings" list.

import type { BindingRow } from './model';

/**
 * Sort key for a key name, so F2 sorts before F10 sorts before Insert.
 *
 * localeCompare on the raw names gives F1, F10, F11, F2. F-keys are instead
 * rewritten to a zero-padded number with prefix "0", every other name gets
 * prefix "1": the prefix puts the F-block ahead of the alphabetic remainder,
 * the padding orders it numerically.
 *
 * 3 digits of padding covers F1-F24 (the native range). parseInt + padStart
 * also normalises spellings like "F007" to "0007", which native never emits.
 *
 * The non-F branch is `"1" + name` with no case folding, leaving the
 * case-insensitive-ish ordering to localeCompare.
 */
export function keyOrder(name: string): string {
  const f = /^F(\d+)$/.exec(name);
  const digits = f?.[1];
  return digits !== undefined ? `0${String(parseInt(digits, 10)).padStart(3, '0')}` : `1${name}`;
}

/**
 * Key order first, owner name as the tie-break.
 *
 * The tie-break is `||`, so names that locale collation treats as equal also
 * fall through to the owner compare; rows are never ordered by label. Two
 * bindings from the same owner on the same key keep input order — stable in
 * every engine we ship on.
 */
export function compareBindings(a: BindingRow, b: BindingRow): number {
  return keyOrder(a.name).localeCompare(keyOrder(b.name)) || a.owner.localeCompare(b.owner);
}
