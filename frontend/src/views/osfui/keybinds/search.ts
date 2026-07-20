// search.ts — the keybinds filter predicate. Pure; belongs in @lib/keybinds
// beside model.ts/sort.ts, pending a follow-up move.

import type { BindingRow } from '@lib/keybinds/model';

/**
 * Filter over the three user-visible strings of a row: key name, action label,
 * owner.
 *
 * `q` must already be trimmed and lowercased — the call sites do that once and
 * this does not repeat it, so a mixed-case query matches nothing.
 *
 * An empty query matches everything (`!q ||` short-circuits), so the
 * unfiltered list is the same code path as a filtered one.
 *
 * Not searched: the input-context label, and for game rows the raw controlmap
 * event id. Both are visible in the row's sub-line, so this is a gap rather
 * than a narrowing; preserved as shipped.
 */
export function matchesQuery(q: string): (b: BindingRow) => boolean {
  return (b) =>
    !q ||
    b.name.toLowerCase().includes(q) ||
    b.label.toLowerCase().includes(q) ||
    b.owner.toLowerCase().includes(q);
}
