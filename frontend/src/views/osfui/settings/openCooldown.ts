/**
 * How long a launched view/card stays inert after the overlay hands off to the
 * target view — long enough to swallow a dead double-click while the launch
 * completes, short enough not to feel stuck. Shared by Detail (view buttons,
 * "Opening…") and Home (cards) so the two launch controls stay in lockstep; each
 * caller keeps its own disabled/label wording.
 */
export const OPEN_COOLDOWN_MS = 1600;
