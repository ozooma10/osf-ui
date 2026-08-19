
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
