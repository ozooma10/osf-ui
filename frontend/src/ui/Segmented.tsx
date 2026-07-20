// The `type:"enum"` control, in both of its forms.
//
// Segmented form requires `widget === "segmented"` and 1 <= options.length <= 5;
// either half failing falls back to a <select>. The upper bound is layout (a
// single row of equal-width buttons; six overflow the control cell); the lower
// bound catches `widget:"segmented"` with no options, which would render an
// empty un-clickable frame instead of an empty dropdown.
//
// Selection is marked with `aria-pressed`: osfui.css styles
// `.osf-segment[aria-pressed="true"]`, so the attribute is the visual.

import { optionLabel } from '@lib/settings/format';
import type { Setting } from '@sdk';

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
            // Nothing reads `data-opt` now, but it ships in the DOM and a
            // third-party stylesheet could key off it, so it stays.
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
    <select
      class="osf-select"
      id={id}
      // A value outside the options selects nothing, so the browser shows the
      // first option. The store cannot hold such a value, but a mock or an
      // older schema can.
      value={value ?? ''}
      disabled={disabled}
      onChange={(e) => onCommit((e.currentTarget as HTMLSelectElement).value)}
    >
      {opts.map((opt) => (
        <option key={opt} value={opt}>
          {optionLabel(setting, opt)}
        </option>
      ))}
    </select>
  );
}
