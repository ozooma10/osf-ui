import { describe, it, expect } from 'vitest';
import type { Setting } from '@sdk';
import {
  DEFAULT_FLOAT_STEP,
  DEFAULT_INT_STEP,
  applyStep,
  clamp,
  hasInvalidStep,
  snap,
  stepDown,
  stepUp,
  stepperFor,
} from '@lib/settings/stepper';

const s = (o: Partial<Setting> & Pick<Setting, 'type'>): Setting => ({ key: 'k', ...o }) as Setting;

describe('stepperFor', () => {
  it('defaults min to 0 and max to 100', () => {
    expect(stepperFor(s({ type: 'int' }))).toMatchObject({ min: 0, max: 100 });
  });

  it('uses ?? so an explicit 0 bound survives', () => {
    const spec = stepperFor(s({ type: 'float', min: 0, max: 0 }));
    expect(spec.min).toBe(0);
    expect(spec.max).toBe(0);
  });

  it('picks the type default step', () => {
    expect(stepperFor(s({ type: 'int' })).step).toBe(DEFAULT_INT_STEP);
    expect(stepperFor(s({ type: 'float' })).step).toBe(DEFAULT_FLOAT_STEP);
  });

  it('falls back to the type default for step 0, negative and NaN', () => {
    // All three would divide-by-zero (or NaN) inside snap and commit NaN over
    // the bridge — the reason the guard is `!(step > 0)` rather than `step < 0`.
    expect(stepperFor(s({ type: 'int', step: 0 })).step).toBe(1);
    expect(stepperFor(s({ type: 'int', step: -5 })).step).toBe(1);
    expect(stepperFor(s({ type: 'float', step: NaN })).step).toBe(0.01);
  });

  it('hasInvalidStep flags exactly those cases and not an absent step', () => {
    expect(hasInvalidStep({ step: 0 })).toBe(true);
    expect(hasInvalidStep({ step: -1 })).toBe(true);
    expect(hasInvalidStep({ step: NaN })).toBe(true);
    expect(hasInvalidStep({ step: 0.5 })).toBe(false);
    expect(hasInvalidStep({})).toBe(false);
  });

  it('does NOT flag a nullish step — `??` gives it the type default silently', () => {
    // The legacy guard runs on the POST-`??` value, so `step: null` resolved to
    // the default and never warned. Warning here would be a new diagnostic.
    const nulled = { step: null } as unknown as Pick<Setting, 'step'>;
    expect(hasInvalidStep(nulled)).toBe(false);
    expect(stepperFor({ type: 'int', ...nulled }).step).toBe(DEFAULT_INT_STEP);
  });
});

describe('snap', () => {
  it('snaps onto the grid measured FROM min, not from zero', () => {
    const spec = stepperFor(s({ type: 'int', min: 1, max: 100, step: 10 }));
    // Grid is 1, 11, 21... — snapping from zero would have given 10.
    expect(snap(spec, 12)).toBe(11);
    expect(snap(spec, 6)).toBe(11);
    expect(snap(spec, 5)).toBe(1);
  });

  it('rounds away IEEE drift for floats so repeated steps stay comparable', () => {
    const spec = stepperFor(s({ type: 'float', min: 0, max: 2, step: 0.1 }));
    let v = 0;
    for (let i = 0; i < 12; i++) v = applyStep(spec, v + spec.step);
    // Without the 1e6 rounding this would be 1.2000000000000002 and the
    // modified dot would light up on a value equal to the default.
    expect(v).toBe(1.2);
  });

  it('rounds int results to whole numbers', () => {
    const spec = stepperFor(s({ type: 'int', min: 0, max: 10, step: 3 }));
    expect(Number.isInteger(snap(spec, 4))).toBe(true);
  });
});

describe('applyStep — SNAP FIRST, CLAMP SECOND', () => {
  it('parks on the bound even when the bound is off the step grid', () => {
    const spec = stepperFor(s({ type: 'int', min: 0, max: 10, step: 3 }));
    // From 9, + asks for 12. snap(12) = 12, clamp = 10 -> parks ON the bound.
    // Clamp-then-snap would give snap(10) = 9 and the + button would look dead.
    expect(applyStep(spec, 12)).toBe(10);
    expect(stepUp(spec, 9)).toBe(10);
  });

  it('parks on the lower bound the same way', () => {
    const spec = stepperFor(s({ type: 'int', min: 1, max: 10, step: 4 }));
    // Grid from min: 1, 5, 9. From 1, − asks for -3 -> snap(-3) = 1 already,
    // so use a min that is off its own grid multiple to exercise the clamp.
    expect(stepDown(spec, 1)).toBe(1);
    expect(applyStep(spec, -100)).toBe(1);
  });

  it('never returns a value outside [min, max]', () => {
    const spec = stepperFor(s({ type: 'float', min: 0.5, max: 1.5, step: 0.25 }));
    for (const v of [-10, 0, 0.6, 1.0, 1.49, 99]) {
      const out = applyStep(spec, v);
      expect(out).toBeGreaterThanOrEqual(0.5);
      expect(out).toBeLessThanOrEqual(1.5);
    }
  });

  it('stepUp/stepDown are a single grid move in the interior', () => {
    const spec = stepperFor(s({ type: 'int', min: 0, max: 100, step: 5 }));
    expect(stepUp(spec, 20)).toBe(25);
    expect(stepDown(spec, 20)).toBe(15);
  });

  it('an off-grid starting value snaps onto the grid on the first press', () => {
    const spec = stepperFor(s({ type: 'int', min: 0, max: 100, step: 10 }));
    // 23 + 10 = 33 -> snaps to 30, so the first press moves less than a full step.
    expect(stepUp(spec, 23)).toBe(30);
  });
});

describe('clamp', () => {
  it('QUIRK: max wins when min > max (Math.min is applied last)', () => {
    const spec = { min: 10, max: 0, step: 1, isInt: true };
    expect(clamp(spec, 5)).toBe(0);
  });
});
