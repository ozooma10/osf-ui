
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
        onChangeCapture={(e) => {
          const raw = (e.currentTarget as HTMLInputElement).value;
          setDragging(null);
          onCommit(spec.isInt ? parseInt(raw, 10) : parseFloat(raw));
        }}
      />
    </>
  );
}
