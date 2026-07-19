// sort.ts — ordering for the "All bindings" list.
//
// Extracted from src/views/osfui/keybinds/main.legacy.js:342-352.

import type { BindingRow } from './model';

/**
 * Sort key for a key NAME, so F2 sorts before F10 sorts before Insert.
 *
 * A plain localeCompare on the raw names would give F1, F10, F11, F2 — so
 * F-keys are rewritten to a zero-padded numeric form and given the prefix
 * "0", while every other name gets the prefix "1". The prefix is what puts
 * the whole F-block ahead of the alphabetic remainder; the padding is what
 * orders the block numerically.
 *
 * Padding is 3 digits, which covers F1-F24 (the whole native range) with
 * room to spare. `parseInt` then `padStart` also normalises exotic spellings
 * like "F007" to "0007" — harmless, since native never emits those.
 *
 * NOTE the non-F branch is `"1" + name` with NO case folding, so localeCompare
 * does the case-insensitive-ish ordering. Preserved as-is.
 */
export function keyOrder(name: string): string {
  const f = /^F(\d+)$/.exec(name);
  const digits = f?.[1];
  return digits !== undefined ? `0${String(parseInt(digits, 10)).padStart(3, '0')}` : `1${name}`;
}

/**
 * The list comparator: key order first, owner name as the tie-break.
 *
 * QUIRK (legacy line 351-352): the tie-break is `||`, so a keyOrder
 * localeCompare of 0 falls through to the owner compare — but a keyOrder
 * compare that returns 0 for DIFFERENT names (locale collation treating them
 * as equal) also falls through, and rows are never ordered by label at all.
 * Two bindings from the same owner on the same key keep their input order,
 * which for Array.prototype.sort is stable in every engine we ship on.
 */
export function compareBindings(a: BindingRow, b: BindingRow): number {
  return keyOrder(a.name).localeCompare(keyOrder(b.name)) || a.owner.localeCompare(b.owner);
}
