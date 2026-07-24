/**
 * How long a launched panel/card stays inert after the overlay hands off to the
 * target view — long enough to swallow a dead double-click while the launch
 * completes, short enough not to feel stuck. Shared by Detail (panel buttons,
 * "Opening…") and Home (cards) so the two launch surfaces stay in lockstep; each
 * caller keeps its own disabled/label wording.
 */
export const OPEN_COOLDOWN_MS = 1600;
