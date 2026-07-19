// conditions.ts — the display-only predicate evaluator behind `visibleWhen` /
// `enabledWhen`.
//
// Ported verbatim from the pre-migration renderer
// (src/views/osfui/settings/main.legacy.js:271-290). Two properties matter more
// than elegance here:
//
//  1. FAIL-CLOSED on an unknown key. A condition that names a key the mod does
//     not (yet) have evaluates FALSE, so the gated row stays hidden/disabled.
//     The alternative (treat it as "no opinion" -> true) would reveal controls
//     a schema deliberately gated behind a setting an older host doesn't know.
//  2. FAIL-OPEN on a malformed condition. Anything that is not an object, and
//     any leaf with no recognised operator, evaluates TRUE — a schema typo must
//     not silently erase a control from the pane.
//
// This is PRESENTATION only. The native SettingsStore never consults it: a
// hidden or disabled control is still validated on write.

import type { Condition, SettingValue } from '@sdk';

/**
 * The value bag a condition is evaluated against — a mod's `values` record.
 *
 * Typed with `| undefined` on purpose: `noUncheckedIndexedAccess` would give us
 * that anyway, and a key can legitimately be PRESENT with an undefined value
 * (see the `in` note in `evalCondition`).
 */
export type ConditionValues = Record<string, SettingValue | undefined>;

/**
 * Optional sink for the "condition references unknown key" diagnostic. The
 * legacy renderer called `console.warn` inline (main.legacy.js:277); this module
 * must stay pure, so the caller injects the reporter instead.
 */
export type UnknownKeyReporter = (key: string) => void;

/**
 * Evaluate a schema condition against a mod's values.
 *
 * `cond` is deliberately typed `unknown`: conditions come off untrusted schema
 * JSON, and the legacy behaviour for junk (return true) is part of the contract.
 */
export function evalCondition(
  cond: unknown,
  values: ConditionValues,
  onUnknownKey?: UnknownKeyReporter,
): boolean {
  // Non-object (null, string, number, undefined) => TRUE. main.legacy.js:272.
  if (!cond || typeof cond !== 'object') return true;

  const c = cond as Record<string, unknown>;

  // Combinators are checked in this exact order, and each only when its payload
  // has the right shape — an `all` that is not an array FALLS THROUGH to `any`,
  // then `not`, then the leaf branch. Reordering changes behaviour for
  // (malformed) conditions that carry more than one combinator key.
  if (Array.isArray(c['all'])) {
    // Empty `all` => true (Array.prototype.every on []).
    return (c['all'] as unknown[]).every((sub) => evalCondition(sub, values, onUnknownKey));
  }
  if (Array.isArray(c['any'])) {
    // Empty `any` => false (Array.prototype.some on []). Asymmetric with `all`
    // by construction, not by accident.
    return (c['any'] as unknown[]).some((sub) => evalCondition(sub, values, onUnknownKey));
  }
  // Truthiness check, NOT a shape check: `not: 0` / `not: ""` skips this branch
  // and falls through to the leaf. Matches main.legacy.js:275.
  if (c['not']) return !evalCondition(c['not'], values, onUnknownKey);

  if (typeof c['key'] === 'string') {
    const key = c['key'];
    // `in`, not hasOwnProperty — carried across from main.legacy.js:277. It
    // walks the prototype chain, so a condition keyed on "toString" or
    // "constructor" reads as KNOWN even against an empty values object and then
    // compares against the inherited function. Preserved: schemas in the wild
    // could (however absurdly) depend on it, and tightening it would flip a
    // fail-closed case to fail-open.
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
    // The numeric comparators coerce the STORED value with Number() but use the
    // condition operand raw — so a string operand compares by JS's relational
    // rules. Faithful to main.legacy.js:282-285.
    if ('gt' in c) return Number(v) > (c['gt'] as number);
    if ('gte' in c) return Number(v) >= (c['gte'] as number);
    if ('lt' in c) return Number(v) < (c['lt'] as number);
    if ('lte' in c) return Number(v) <= (c['lte'] as number);
    // `truthy: false` means "assert falsy", not "no opinion".
    if ('truthy' in c) return c['truthy'] ? !!v : !v;

    // A leaf with a known key but no operator: no constraint => TRUE.
    return true;
  }

  // Neither a combinator nor a keyed leaf => TRUE.
  return true;
}

/**
 * Convenience wrapper for the common "gate is absent => visible/enabled" call
 * shape. `undefined` (no `visibleWhen` authored) is TRUE, same as the legacy
 * `if (lr.visibleWhen)` guard at main.legacy.js:1438.
 */
export function evalGate(
  cond: Condition | undefined,
  values: ConditionValues,
  onUnknownKey?: UnknownKeyReporter,
): boolean {
  if (cond === undefined) return true;
  return evalCondition(cond, values, onUnknownKey);
}
