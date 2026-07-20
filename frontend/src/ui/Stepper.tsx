// Stepper.tsx — the − / + variant of a numeric setting (`widget:"stepper"`).
//
// A separate markup path from the slider, not a variation on it. The slider hands
// the row a `.osf-value` readout placed before the control; the stepper carries its
// readout inside itself as `.osf-stepper-val` and hands the row nothing. `.osf-stepper`
// lays its three children out as a grid, so a `.osf-value` here would land outside
// the stepper frame as an orphan readout.
//
// Arithmetic is @lib/settings/stepper's: snap onto the step grid measured from `min`,
// then clamp. See `applyStep` there for why that order isn't interchangeable.

import { stepDown, stepUp, type StepperSpec } from '@lib/settings/stepper';
import { formatNumber, type NumberFormatSource } from '@lib/settings/format';

export interface StepperProps {
  /** Goes on the wrapper, so `label[for]` targets it. */
  id: string;
  spec: StepperSpec;
  /** Only `type` and `format` are read. */
  setting: NumberFormatSource;
  /** Undefined (no stored value) starts at `min`. */
  value: number | undefined;
  disabled: boolean;
  onCommit: (next: number) => void;
}

export function Stepper({ id, spec, setting, value, disabled, onCommit }: StepperProps) {
  // Derived from the model, not held in a closure: every commit applies
  // optimistically before the native ack, so an external writer's push moves the
  // stepper instead of being overwritten by a stale value on the next click.
  const current = value ?? spec.min;

  return (
    <div class="osf-stepper" id={id}>
      {/* U+2212 MINUS SIGN, not a hyphen — optically matches the "+". */}
      <button
        type="button"
        class="osf-stepper-btn"
        disabled={disabled}
        onClick={() => onCommit(stepDown(spec, current))}
      >
        −
      </button>
      <span class="osf-stepper-val">{formatNumber(setting, current)}</span>
      <button
        type="button"
        class="osf-stepper-btn"
        disabled={disabled}
        onClick={() => onCommit(stepUp(spec, current))}
      >
        +
      </button>
    </div>
  );
}
