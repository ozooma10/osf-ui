// Segmented.tsx — the `type:"enum"` control, in both of its forms.
//
// Ports `buildEnum` (settings/main.legacy.js:399-426). The form is chosen by a
// TWO-PART test that must be reproduced exactly:
//
//   widget === "segmented"  AND  1 <= options.length <= 5
//
// Either half failing falls back to a <select>. The upper bound is a layout
// constraint (a segmented control is a single row of equal-width buttons; six
// of them overflow the control cell), and the lower bound catches a schema that
// declares `widget:"segmented"` with no options at all — which would otherwise
// render an empty, un-clickable frame instead of an empty dropdown.
//
// Note the segmented form marks selection with `aria-pressed` on each button,
// like the kit's switch: `.osf-segment[aria-pressed="true"]` is what osfui.css
// styles, so the attribute is the visual.

import { optionLabel } from '@lib/settings/format';
import type { Setting } from '@sdk';

/** Just the fields both forms read. */
export type EnumSource = Pick<Setting, 'options' | 'optionLabels' | 'widget'>;

/** main.legacy.js:401 — the segmented-vs-select decision, isolated for testing. */
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
            // `data-opt` is how legacy re-found each button to update the
            // pressed state (main.legacy.js:405). Nothing reads it now, but it
            // ships in the DOM today and a third-party stylesheet could key off
            // it, so it stays.
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
      // A value that is not among the options selects nothing, so the browser
      // shows the first option — exactly what legacy's "no <option selected>"
      // produced. The store cannot hold such a value, but a mock or an older
      // schema can.
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
