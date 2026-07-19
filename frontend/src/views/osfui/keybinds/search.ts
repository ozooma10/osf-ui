// search.ts — the keybinds filter predicate.
//
// Extracted from main.legacy.js:271-276. It lives beside the view rather than
// in @lib/keybinds only because @lib is closed to this migration; it is pure,
// has no view dependencies, and belongs next to model.ts/sort.ts. Flagged for
// the follow-up move.

import type { BindingRow } from '@lib/keybinds/model';

/**
 * Build a filter over the three user-visible strings of a row: the key name,
 * the action label, and the owner.
 *
 * `q` must ALREADY be trimmed and lowercased — both call sites do
 * `searchEl.value.trim().toLowerCase()` once (legacy 245, 349) and this does
 * not repeat it, so a mixed-case query silently matches nothing.
 *
 * An empty query matches everything (`!q ||` short-circuits first), which is
 * what makes the unfiltered list the same code path as a filtered one.
 *
 * NOT searched: the input-context label, and — for game rows — the raw
 * controlmap event id. Both are visible in the row's sub-line, so this is a
 * genuine gap rather than a deliberate narrowing; preserved as shipped.
 */
export function matchesQuery(q: string): (b: BindingRow) => boolean {
  return (b) =>
    !q ||
    b.name.toLowerCase().includes(q) ||
    b.label.toLowerCase().includes(q) ||
    b.owner.toLowerCase().includes(q);
}
