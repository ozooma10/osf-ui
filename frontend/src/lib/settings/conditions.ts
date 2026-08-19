
import type { Condition, SettingValue } from '@sdk';

export type ConditionValues = Record<string, SettingValue | undefined>;

export type UnknownKeyReporter = (key: string) => void;

export function evalCondition(
  cond: unknown,
  values: ConditionValues,
  onUnknownKey?: UnknownKeyReporter,
): boolean {
  // Non-object (null, string, number, undefined) => true.
  if (!cond || typeof cond !== 'object') return true;

  const c = cond as Record<string, unknown>;

  if (Array.isArray(c['all'])) {
    // Empty `all` => true (every on []).
    return (c['all'] as unknown[]).every((sub) => evalCondition(sub, values, onUnknownKey));
  }
  if (Array.isArray(c['any'])) {
    // Empty `any` => false (some on []). Asymmetric with `all` by construction.
    return (c['any'] as unknown[]).some((sub) => evalCondition(sub, values, onUnknownKey));
  }
  if (c['not']) return !evalCondition(c['not'], values, onUnknownKey);

  if (typeof c['key'] === 'string') {
    const key = c['key'];
    if (!(key in values)) {
      onUnknownKey?.(key);
      return false;
    }
    const v = values[key];

    if ('eq' in c) return v === c['eq'];
    if ('ne' in c) return v !== c['ne'];
    if ('in' in c) return Array.isArray(c['in']) && (c['in'] as unknown[]).includes(v);
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
