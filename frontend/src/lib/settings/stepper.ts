// stepper.ts — the int/float stepper arithmetic (main.legacy.js:353-388).
//
// The − / + buttons do NOT simply add `step` to the stored value. They snap the
// result onto the step grid measured FROM `min`, then clamp. Both halves matter
// and the ORDER matters; see `applyStep`.

import type { Setting } from '@sdk';

/** Type defaults for a missing/invalid `step` (main.legacy.js:359). */
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
 * Was the schema's `step` unusable? Exposed so the caller can emit the
 * dev warning the legacy renderer logged inline (main.legacy.js:360) without
 * this module touching `console`.
 */
export function hasInvalidStep(setting: Pick<Setting, 'step'>): boolean {
  const declared = setting.step;
  // `!= null`, NOT `!== undefined`: `stepperFor` resolves the step with `??`,
  // so a `step: null` is NULLISH and silently takes the type default — the
  // legacy renderer never warned about it (main.legacy.js:359-360 evaluates the
  // guard on the POST-`??` value, which is already the default by then).
  // Warning on null would be a diagnostic the shipped view never emitted.
  if (declared == null) return false;
  // The legacy guard is `!(step > 0)`, which is true for 0, negatives AND NaN
  // in one expression — a NaN step would divide-by-zero in `snap` and commit
  // NaN over the bridge.
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

/** main.legacy.js:370 — note `Math.min(max, ...)` wins when min > max. */
export function clamp(spec: StepperSpec, v: number): number {
  return Math.min(spec.max, Math.max(spec.min, v));
}

/**
 * Snap onto the step grid measured from `min`, so a stored value that started
 * off-grid still lands on grid, and round away IEEE drift so repeated float
 * steps stay STRUCTURALLY comparable to the schema default (1.2, not
 * 1.2000000000000002 — otherwise the modified dot lights up on a value that is
 * numerically the default). main.legacy.js:374-377.
 */
export function snap(spec: StepperSpec, v: number): number {
  const s = spec.min + Math.round((v - spec.min) / spec.step) * spec.step;
  return spec.isInt ? Math.round(s) : Math.round(s * 1e6) / 1e6;
}

/**
 * What the − / + buttons commit: SNAP FIRST, CLAMP SECOND
 * (`clamp(snap(v))`, main.legacy.js:379).
 *
 * The order is load-bearing and is NOT the same as clamp-then-snap. With
 * min:0 max:10 step:3, pressing + from 9 asks for 12: snap(12) = 12, clamp = 10
 * — the value parks on the BOUND, off the step grid. Clamp-then-snap would give
 * snap(10) = 9 and the + button would appear dead at the top of the range.
 * Parking on the bound is the intended UX; do not "fix" it.
 */
export function applyStep(spec: StepperSpec, v: number): number {
  return clamp(spec, snap(spec, v));
}

/** Convenience: one decrement / increment from the current value. */
export function stepDown(spec: StepperSpec, current: number): number {
  return applyStep(spec, current - spec.step);
}
export function stepUp(spec: StepperSpec, current: number): number {
  return applyStep(spec, current + spec.step);
}
