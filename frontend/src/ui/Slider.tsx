// Slider.tsx — the range variant of a numeric setting (int / float).
//
// Ports the slider branch of `buildRange` (settings/main.legacy.js:390-396).
//
// THE EVENT SPLIT IS THE WHOLE POINT:
//
//   input   repaints the `.osf-value` readout ONLY
//   change  commits
//
// A range input fires `input` continuously while the thumb is dragged. If the
// commit hung off `input` the view would send one `settings.set` per pixel of
// travel, each one a bridge round trip the native store validates, persists
// (write-behind) and echoes back — the classic MCM slider spam. `change` fires
// once, when the drag ends. Do not "simplify" these into one handler.
//
// This component renders BOTH the readout and the input as siblings, because
// the control cell's order is `[reset ↺][readout][control]` and the readout is
// a separate node in it (main.legacy.js:605-606). The stepper variant has no
// such node at all — see Stepper.tsx.

import { useState } from 'preact/hooks';
import type { StepperSpec } from '@lib/settings/stepper';
import { formatNumber, type NumberFormatSource } from '@lib/settings/format';

export interface SliderProps {
  id: string;
  spec: StepperSpec;
  setting: NumberFormatSource;
  value: number | undefined;
  disabled: boolean;
  onCommit: (next: number) => void;
}

export function Slider({ id, spec, setting, value, disabled, onCommit }: SliderProps) {
  // The in-flight drag position, as the raw string the DOM reports. Null when
  // not dragging, so the model is the source of truth the rest of the time and
  // an external writer's push moves the thumb.
  //
  // Legacy needed no such state: its input was uncontrolled, so the DOM held
  // the drag position by itself. A controlled Preact input would be yanked back
  // to the model value on every `input` re-render without this.
  const [dragging, setDragging] = useState<string | null>(null);

  const modelValue = value ?? spec.min;
  const shown = dragging ?? String(modelValue);

  return (
    <>
      {/* Legacy formats the READOUT from the raw DOM string during a drag
          (`formatNumber(setting, slider.value)`, main.legacy.js:393), relying on
          formatNumber's `Number(v)` coercion. Kept: `shown` is that same
          string. */}
      <span class="osf-value">{formatNumber(setting, shown)}</span>
      <input
        type="range"
        class="osf-range"
        id={id}
        min={spec.min}
        max={spec.max}
        step={spec.step}
        value={shown}
        disabled={disabled}
        onInput={(e) => setDragging((e.currentTarget as HTMLInputElement).value)}
        onChange={(e) => {
          const raw = (e.currentTarget as HTMLInputElement).value;
          setDragging(null);
          // parseInt/parseFloat by declared TYPE, not by whether the string
          // looks integral: an int setting with step 1 must commit 3, never
          // "3" or 3.0 — the store's `is_number_integer()` check refuses a
          // float for an int setting (main.legacy.js:395).
          onCommit(spec.isInt ? parseInt(raw, 10) : parseFloat(raw));
        }}
      />
    </>
  );
}
