/**
 * Join class names, skipping falsy parts, IN ARGUMENT ORDER.
 *
 * Convention: base classes first, conditional state class last — exactly as the
 * hand-written ternaries did. Be precise about why, because the obvious reason is
 * wrong: neither CSS matching nor `querySelector` cares about the ORDER of names
 * inside a `class` attribute. padnav reads classes in two places and both are
 * order-blind — `el.closest('.row')` for banding (padnav.js:79, the load-bearing
 * one, documented on Row) and `document.querySelector('.listening')` as a
 * presence test to suspend navigation during a capture (padnav.js:184). It never
 * queries `.pending` at all. So the order is NOT a padnav contract.
 *
 * It is load-bearing for the DOM-contract tests, which compare `className`
 * verbatim to catch a component that silently stops emitting the shape padnav
 * queries — see `frontend/test/dom-contracts.test.tsx`, which covers Row plus
 * KeyField (`.listening`) and ActionButton (`.pending`). Note's `hidden-cond`
 * uses the same trailing-state-class idiom and is deliberately not asserted: no
 * padnav or test consumer reads it. Keeping one order across the kit is what
 * makes those assertions writable at all.
 *
 * The falsy skip is what removes the duplication these replaced: each ternary
 * had to spell the whole base list out twice, once per branch.
 */
export function cx(...parts: Array<string | false | null | undefined>): string {
  return parts.filter(Boolean).join(' ');
}
