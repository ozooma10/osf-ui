// Stepper.tsx — the − / + variant of a numeric setting (`widget:"stepper"`).
//
// Ports the stepper branch of `buildRange` (settings/main.legacy.js:365-388).
//
// THIS IS A DIFFERENT MARKUP PATH FROM THE SLIDER, not a variation on it. The
// slider hands the row a separate `.osf-value` readout that the control cell
// places BEFORE the control; the stepper carries its readout INSIDE itself as
// `.osf-stepper-val` and hands the row nothing (`{ control: wrap, value: null }`,
// main.legacy.js:387). `.osf-stepper` lays its three children out as a grid, so
// reusing `.osf-value` here would produce markup the CSS does not expect — an
// orphan readout outside the stepper frame.
//
// The arithmetic is @lib/settings/stepper's: snap onto the step grid measured
// from `min`, THEN clamp. See `applyStep` there for why that order is not
// interchangeable.

import { stepDown, stepUp, type StepperSpec } from '@lib/settings/stepper';
import { formatNumber, type NumberFormatSource } from '@lib/settings/format';

export interface StepperProps {
  /** Goes on the WRAPPER (main.legacy.js:386), so `label[for]` targets it. */
  id: string;
  spec: StepperSpec;
  /** Only `type` and `format` are read — the display formatting source. */
  setting: NumberFormatSource;
  /** Undefined (no stored value) starts at `min`, as legacy's `current ?? min`. */
  value: number | undefined;
  disabled: boolean;
  onCommit: (next: number) => void;
}

export function Stepper({ id, spec, setting, value, disabled, onCommit }: StepperProps) {
  // Legacy held this in a closure (`let val`) and stepped from the CLOSURE
  // value, not the model. The two are always equal because every commit applies
  // optimistically to the local model before the native ack, so deriving is
  // equivalent — and it means an external writer's push actually moves the
  // stepper instead of being overwritten by a stale closure on the next click.
  const current = value ?? spec.min;

  return (
    <div class="osf-stepper" id={id}>
      {/* U+2212 MINUS SIGN, not a hyphen — it is the glyph the shipped view
          draws and it optically matches the "+". */}
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
