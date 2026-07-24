/**
 * Join class names, skipping falsy parts, IN ARGUMENT ORDER.
 *
 * Order is a contract, not a detail: padnav selects on trailing state classes
 * (`.listening` on an armed KeyField suspends arrow navigation; `.pending` on a
 * running ActionButton), and the kit's CSS is written expecting the base classes
 * first. So pass the base classes first and the conditional state class last —
 * exactly as the hand-written ternaries did.
 *
 * The falsy skip is what removes the duplication these replaced: each ternary
 * had to spell the whole base list out twice, once per branch.
 */
export function cx(...parts: Array<string | false | null | undefined>): string {
  return parts.filter(Boolean).join(' ');
}
