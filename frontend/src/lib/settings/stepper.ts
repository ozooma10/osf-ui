
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

export function hasInvalidStep(setting: Pick<Setting, 'step'>): boolean {
  const declared = setting.step;
  if (declared == null) return false;
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

export function snap(spec: StepperSpec, v: number): number {
  const s = spec.min + Math.round((v - spec.min) / spec.step) * spec.step;
  return spec.isInt ? Math.round(s) : Math.round(s * 1e6) / 1e6;
}

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
