import { describe, it, expect } from 'vitest';
import { evalCondition, evalGate } from '@lib/settings/conditions';
import type { ConditionValues } from '@lib/settings/conditions';

const values: ConditionValues = {
  enabled: true,
  off: false,
  count: 5,
  zero: 0,
  name: 'alpha',
  empty: '',
  flags: ['a', 'b'],
};

describe('evalCondition — fail-open on junk', () => {
  it('treats a non-object condition as TRUE', () => {
    expect(evalCondition(null, values)).toBe(true);
    expect(evalCondition(undefined, values)).toBe(true);
    expect(evalCondition('enabled', values)).toBe(true);
    expect(evalCondition(0, values)).toBe(true);
    expect(evalCondition(42, values)).toBe(true);
  });

  it('treats an object with no combinator and no key as TRUE', () => {
    expect(evalCondition({}, values)).toBe(true);
    expect(evalCondition({ eq: 1 }, values)).toBe(true);
  });

  it('treats a keyed leaf with no operator as TRUE', () => {
    expect(evalCondition({ key: 'enabled' }, values)).toBe(true);
  });
});

describe('evalCondition — fail-CLOSED on an unknown key', () => {
  it('returns false and reports the key', () => {
    const seen: string[] = [];
    expect(evalCondition({ key: 'nope', eq: 1 }, values, (k) => seen.push(k))).toBe(false);
    expect(seen).toEqual(['nope']);
  });

  it('fails closed even for an operator that would otherwise be TRUE', () => {
    // `truthy: false` against a missing key must NOT read as "absent is falsy".
    expect(evalCondition({ key: 'nope', truthy: false }, values)).toBe(false);
    // ...and neither does `ne`, which would be true against undefined.
    expect(evalCondition({ key: 'nope', ne: 'anything' }, values)).toBe(false);
  });

  it('fails closed against a completely empty value bag', () => {
    expect(evalCondition({ key: 'enabled', truthy: true }, {})).toBe(false);
  });

  it('does not report a known key', () => {
    const seen: string[] = [];
    evalCondition({ key: 'enabled', truthy: true }, values, (k) => seen.push(k));
    expect(seen).toEqual([]);
  });

  it('QUIRK: `in` walks the prototype chain, so "toString" reads as KNOWN', () => {
    // Legacy uses `cond.key in values` (main.legacy.js:277). Documented, not
    // desired — a fail-closed case flipping open would be the regression.
    expect(evalCondition({ key: 'toString', truthy: true }, {})).toBe(true);
    expect(evalCondition({ key: 'toString', truthy: false }, {})).toBe(false);
  });
});

describe('evalCondition — leaf operators', () => {
  it('eq / ne compare strictly', () => {
    expect(evalCondition({ key: 'count', eq: 5 }, values)).toBe(true);
    expect(evalCondition({ key: 'count', eq: '5' }, values)).toBe(false);
    expect(evalCondition({ key: 'count', ne: 5 }, values)).toBe(false);
    expect(evalCondition({ key: 'count', ne: 6 }, values)).toBe(true);
  });

  it('eq works for falsy operands (presence test, not truthiness)', () => {
    expect(evalCondition({ key: 'off', eq: false }, values)).toBe(true);
    expect(evalCondition({ key: 'zero', eq: 0 }, values)).toBe(true);
    expect(evalCondition({ key: 'empty', eq: '' }, values)).toBe(true);
  });

  it('in requires an array operand', () => {
    expect(evalCondition({ key: 'name', in: ['alpha', 'beta'] }, values)).toBe(true);
    expect(evalCondition({ key: 'name', in: ['beta'] }, values)).toBe(false);
    expect(evalCondition({ key: 'name', in: 'alpha' }, values)).toBe(false);
  });

  it('in compares by identity, so an array value never matches', () => {
    expect(evalCondition({ key: 'flags', in: [['a', 'b']] }, values)).toBe(false);
  });

  it('gt / gte / lt / lte coerce the stored value with Number()', () => {
    expect(evalCondition({ key: 'count', gt: 4 }, values)).toBe(true);
    expect(evalCondition({ key: 'count', gt: 5 }, values)).toBe(false);
    expect(evalCondition({ key: 'count', gte: 5 }, values)).toBe(true);
    expect(evalCondition({ key: 'count', lt: 6 }, values)).toBe(true);
    expect(evalCondition({ key: 'count', lte: 5 }, values)).toBe(true);
    // true -> 1, so `gt: 0` holds for a bool.
    expect(evalCondition({ key: 'enabled', gt: 0 }, values)).toBe(true);
    // "alpha" -> NaN, and every comparison against NaN is false.
    expect(evalCondition({ key: 'name', gt: 0 }, values)).toBe(false);
    expect(evalCondition({ key: 'name', lt: 0 }, values)).toBe(false);
  });

  it('truthy: false asserts falsiness rather than meaning "no opinion"', () => {
    expect(evalCondition({ key: 'enabled', truthy: true }, values)).toBe(true);
    expect(evalCondition({ key: 'enabled', truthy: false }, values)).toBe(false);
    expect(evalCondition({ key: 'zero', truthy: false }, values)).toBe(true);
    expect(evalCondition({ key: 'empty', truthy: false }, values)).toBe(true);
    // An empty array is truthy in JS — a flags setting with nothing selected
    // still reads as "truthy".
    expect(evalCondition({ key: 'flags', truthy: true }, { flags: [] })).toBe(true);
  });

  it('applies exactly one operator, in declaration order', () => {
    // `eq` wins over `ne`, so a leaf carrying both is decided by `eq` alone.
    expect(evalCondition({ key: 'count', eq: 5, ne: 5 }, values)).toBe(true);
  });
});

describe('evalCondition — combinators', () => {
  it('all requires every branch', () => {
    const c = { all: [{ key: 'enabled', truthy: true }, { key: 'count', gte: 5 }] };
    expect(evalCondition(c, values)).toBe(true);
    expect(evalCondition({ all: [c, { key: 'off', truthy: true }] }, values)).toBe(false);
  });

  it('any requires one branch', () => {
    expect(evalCondition({ any: [{ key: 'off', truthy: true }, { key: 'enabled', truthy: true }] }, values)).toBe(true);
    expect(evalCondition({ any: [{ key: 'off', truthy: true }] }, values)).toBe(false);
  });

  it('empty all is TRUE and empty any is FALSE (asymmetric by construction)', () => {
    expect(evalCondition({ all: [] }, values)).toBe(true);
    expect(evalCondition({ any: [] }, values)).toBe(false);
  });

  it('not inverts', () => {
    expect(evalCondition({ not: { key: 'enabled', truthy: true } }, values)).toBe(false);
    expect(evalCondition({ not: { key: 'off', truthy: true } }, values)).toBe(true);
  });

  it('not of an unknown key inverts the fail-closed FALSE into TRUE', () => {
    // The fail-closed rule is about the LEAF, so negation still applies.
    expect(evalCondition({ not: { key: 'nope', truthy: true } }, values)).toBe(true);
  });

  it('a non-array `all` falls through to the next branch', () => {
    // `all` is junk, so evaluation continues to `any`.
    expect(evalCondition({ all: 'yes', any: [{ key: 'enabled', truthy: true }] }, values)).toBe(true);
    // ...and with nothing left to try, to the keyed leaf.
    expect(evalCondition({ all: 'yes', key: 'off', truthy: true }, values)).toBe(false);
  });

  it('QUIRK: `not` is a truthiness check, so `not: 0` falls through to the leaf', () => {
    expect(evalCondition({ not: 0, key: 'off', truthy: true }, values)).toBe(false);
    expect(evalCondition({ not: '', key: 'enabled', truthy: true }, values)).toBe(true);
  });

  it('propagates the unknown-key reporter through combinators', () => {
    const seen: string[] = [];
    evalCondition({ all: [{ not: { key: 'deep', eq: 1 } }] }, values, (k) => seen.push(k));
    expect(seen).toEqual(['deep']);
  });
});

describe('evalGate', () => {
  it('treats an absent gate as TRUE', () => {
    expect(evalGate(undefined, values)).toBe(true);
  });
  it('delegates a present gate', () => {
    expect(evalGate({ key: 'off', truthy: true }, values)).toBe(false);
  });
});
