// Display-only predicate evaluator behind `visibleWhen` / `enabledWhen`.
//
// Two invariants:
//  1. Fail-closed on an unknown key: a condition naming a key the mod does not
//     have evaluates false, so the gated row stays hidden/disabled rather than
//     exposing a control an older host doesn't know about.
//  2. Fail-open on a malformed condition: a non-object, or a leaf with no
//     recognised operator, evaluates true so a schema typo can't erase a control.
//
// Presentation only. The native SettingsStore never consults this; a hidden or
// disabled control is still validated on write.

import type { Condition, SettingValue } from '@sdk';

/**
 * A mod's `values` record. `| undefined` because a key can be present with an
 * undefined value (see the `in` note in `evalCondition`).
 */
export type ConditionValues = Record<string, SettingValue | undefined>;

/**
 * Sink for the "condition references unknown key" diagnostic. Injected so this
 * module stays pure.
 */
export type UnknownKeyReporter = (key: string) => void;

/**
 * Evaluate a schema condition against a mod's values.
 *
 * `cond` is `unknown` because conditions come off untrusted schema JSON;
 * returning true for junk is part of the contract.
 */
export function evalCondition(
  cond: unknown,
  values: ConditionValues,
  onUnknownKey?: UnknownKeyReporter,
): boolean {
  // Non-object (null, string, number, undefined) => true.
  if (!cond || typeof cond !== 'object') return true;

  const c = cond as Record<string, unknown>;

  // Combinators are checked in this order, each only when its payload has the
  // right shape — an `all` that is not an array falls through to `any`, then
  // `not`, then the leaf branch. Reordering changes behaviour for malformed
  // conditions carrying more than one combinator key.
  if (Array.isArray(c['all'])) {
    // Empty `all` => true (every on []).
    return (c['all'] as unknown[]).every((sub) => evalCondition(sub, values, onUnknownKey));
  }
  if (Array.isArray(c['any'])) {
    // Empty `any` => false (some on []). Asymmetric with `all` by construction.
    return (c['any'] as unknown[]).some((sub) => evalCondition(sub, values, onUnknownKey));
  }
  // Truthiness check, not a shape check: `not: 0` / `not: ""` skips this branch
  // and falls through to the leaf.
  if (c['not']) return !evalCondition(c['not'], values, onUnknownKey);

  if (typeof c['key'] === 'string') {
    const key = c['key'];
    // `in`, not hasOwnProperty: it walks the prototype chain, so a condition
    // keyed on "toString" or "constructor" reads as known even against an empty
    // values object and compares against the inherited function. Tightening it
    // would flip a fail-closed case to fail-open.
    if (!(key in values)) {
      onUnknownKey?.(key);
      return false;
    }
    const v = values[key];

    // Exactly one operator per leaf, tested in declaration order. `in cond`
    // (not a truthiness test) so `eq: false` / `eq: 0` / `eq: ""` work.
    if ('eq' in c) return v === c['eq'];
    if ('ne' in c) return v !== c['ne'];
    if ('in' in c) return Array.isArray(c['in']) && (c['in'] as unknown[]).includes(v);
    // Numeric comparators coerce the stored value with Number() but use the
    // condition operand raw, so a string operand compares by JS relational rules.
    if ('gt' in c) return Number(v) > (c['gt'] as number);
    if ('gte' in c) return Number(v) >= (c['gte'] as number);
    if ('lt' in c) return Number(v) < (c['lt'] as number);
    if ('lte' in c) return Number(v) <= (c['lte'] as number);
    // `truthy: false` means "assert falsy", not "no opinion".
    if ('truthy' in c) return c['truthy'] ? !!v : !v;

    // Known key, no operator: no constraint => true.
    return true;
  }

  // Neither a combinator nor a keyed leaf => true.
  return true;
}

/** Gate wrapper: an absent condition (no `visibleWhen` authored) is true. */
export function evalGate(
  cond: Condition | undefined,
  values: ConditionValues,
  onUnknownKey?: UnknownKeyReporter,
): boolean {
  if (cond === undefined) return true;
  return evalCondition(cond, values, onUnknownKey);
}
