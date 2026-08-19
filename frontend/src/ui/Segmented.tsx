
import { optionLabel } from '@lib/settings/format';
import type { Setting } from '@sdk';
import { Dropdown } from './Dropdown';

/** Just the fields both forms read. */
export type EnumSource = Pick<Setting, 'options' | 'optionLabels' | 'widget'>;

/** The segmented-vs-select decision, isolated for testing. */
export function isSegmented(setting: EnumSource): boolean {
  const opts = setting.options || [];
  return setting.widget === 'segmented' && opts.length > 0 && opts.length <= 5;
}

export interface SegmentedProps {
  id: string;
  setting: EnumSource;
  /** The stored option, or undefined when the mod has no value for the key. */
  value: string | undefined;
  disabled: boolean;
  onCommit: (next: string) => void;
}

export function Segmented({ id, setting, value, disabled, onCommit }: SegmentedProps) {
  const opts = setting.options || [];

  if (isSegmented(setting)) {
    return (
      <div class="osf-segmented" id={id} role="group">
        {opts.map((opt) => (
          <button
            key={opt}
            type="button"
            class="osf-segment"
            data-opt={opt}
            aria-pressed={opt === value ? 'true' : 'false'}
            disabled={disabled}
            onClick={() => onCommit(opt)}
          >
            {optionLabel(setting, opt)}
          </button>
        ))}
      </div>
    );
  }

  return (
    <Dropdown
      id={id}
      value={value}
      options={opts.map((opt) => ({ value: opt, label: optionLabel(setting, opt) }))}
      disabled={disabled}
      onCommit={onCommit}
    />
  );
}
