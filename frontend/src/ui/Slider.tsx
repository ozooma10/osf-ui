// The range variant of a numeric setting (int / float).
//
// The event split is load-bearing:
//   input   repaints the `.osf-value` readout only
//   change  commits
//
// A range input fires `input` continuously while the thumb is dragged. Committing
// on `input` would send one `settings.set` per pixel of travel, each a bridge
// round trip the native store validates, persists (write-behind) and echoes back
// — the classic MCM slider spam. `change` fires once, when the drag ends. Do not
// merge these into one handler.
//
// Readout and input render as siblings because the control cell's order is
// `[reset ↺][readout][control]` and the readout is a separate node in it. The
// stepper variant has no such node — see Stepper.tsx.

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
  // In-flight drag position, as the raw string the DOM reports. Null when not
  // dragging, so the model is the source of truth the rest of the time and an
  // external writer's push moves the thumb. Without this the controlled input
  // is yanked back to the model value on every `input` re-render.
  const [dragging, setDragging] = useState<string | null>(null);

  const modelValue = value ?? spec.min;
  const shown = dragging ?? String(modelValue);

  return (
    <>
      {/* The readout formats from the raw DOM string during a drag, relying on
          formatNumber's `Number(v)` coercion. */}
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
          // parseInt/parseFloat by declared type, not by whether the string
          // looks integral: an int setting must commit 3, never "3" or 3.0 —
          // the store's `is_number_integer()` check refuses a float for an
          // int setting.
          onCommit(spec.isInt ? parseInt(raw, 10) : parseFloat(raw));
        }}
      />
    </>
  );
}
