// Int/float stepper arithmetic. The − / + buttons do not just add `step` to the
// stored value: they snap the result onto the step grid measured from `min`,
// then clamp. The order matters; see `applyStep`.

import type { Setting } from '@sdk';

/** Type defaults for a missing/invalid `step`. */
export const DEFAULT_INT_STEP = 1;
export const DEFAULT_FLOAT_STEP = 0.01;

/** Everything the stepper needs, resolved from a schema setting. */
export interface StepperSpec {
  min: number;
  max: number;
  step: number;
  isInt: boolean;
}

/**
 * Was the schema's `step` unusable? Separate from `stepperFor` so the caller
 * owns the dev warning and this module never touches `console`.
 */
export function hasInvalidStep(setting: Pick<Setting, 'step'>): boolean {
  const declared = setting.step;
  // `!= null`, not `!== undefined`: `stepperFor` resolves the step with `??`,
  // so a null step is nullish and silently takes the type default. Warning on
  // it would be a diagnostic the shipped view never emitted.
  if (declared == null) return false;
  // `!(step > 0)` catches 0, negatives and NaN in one expression — a NaN step
  // divides by zero in `snap` and commits NaN over the bridge.
  return !(declared > 0);
}

export function stepperFor(setting: Pick<Setting, 'type' | 'min' | 'max' | 'step'>): StepperSpec {
  const isInt = setting.type === 'int';
  // `??` not `||`: min:0 / max:0 are legitimate bounds.
  const min = setting.min ?? 0;
  const max = setting.max ?? 100;
  let step = setting.step ?? (isInt ? DEFAULT_INT_STEP : DEFAULT_FLOAT_STEP);
  if (!(step > 0)) step = isInt ? DEFAULT_INT_STEP : DEFAULT_FLOAT_STEP;
  return { min, max, step, isInt };
}

/** `Math.min(max, ...)` wins when min > max. */
export function clamp(spec: StepperSpec, v: number): number {
  return Math.min(spec.max, Math.max(spec.min, v));
}

/**
 * Snap onto the step grid measured from `min`, so a value that started off-grid
 * lands on grid. Floats round to 1e-6 to shed IEEE drift: repeated steps must
 * stay structurally comparable to the schema default (1.2, not
 * 1.2000000000000002), or the modified dot lights up on a value that is
 * numerically the default.
 */
export function snap(spec: StepperSpec, v: number): number {
  const s = spec.min + Math.round((v - spec.min) / spec.step) * spec.step;
  return spec.isInt ? Math.round(s) : Math.round(s * 1e6) / 1e6;
}

/**
 * What the − / + buttons commit: snap first, clamp second. The order is
 * load-bearing. With min:0 max:10 step:3, + from 9 asks for 12: snap(12) = 12,
 * clamp = 10, so the value parks on the bound, off the step grid.
 * Clamp-then-snap would give snap(10) = 9 and the + button would look dead at
 * the top of the range. Parking on the bound is intended; do not "fix" it.
 */
export function applyStep(spec: StepperSpec, v: number): number {
  return clamp(spec, snap(spec, v));
}

/** One decrement / increment from the current value. */
export function stepDown(spec: StepperSpec, current: number): number {
  return applyStep(spec, current - spec.step);
}
export function stepUp(spec: StepperSpec, current: number): number {
  return applyStep(spec, current + spec.step);
}
