
import { optionLabel } from '@lib/settings/format';
import type { Setting } from '@sdk';

export type FlagsSource = Pick<Setting, 'options' | 'optionLabels'>;

export interface FlagsProps {
  id: string;
  setting: FlagsSource;
  /** The stored array. Anything that is not an array reads as "none selected". */
  value: readonly string[] | undefined;
  disabled: boolean;
  onCommit: (next: string[]) => void;
}

export function Flags({ id, setting, value, disabled, onCommit }: FlagsProps) {
  const opts = (setting.options || []).filter((o): o is string => typeof o === 'string');
  const selected = new Set(
    Array.isArray(value) ? value.filter((v): v is string => typeof v === 'string') : [],
  );

  const toggle = (opt: string, checked: boolean) => {
    const next = new Set(selected);
    if (checked) next.add(opt);
    else next.delete(opt);
    onCommit(opts.filter((o) => next.has(o)));
  };

  return (
    <div class="osf-flags" id={id} role="group">
      {opts.map((opt) => (
        <label key={opt} class="osf-flag">
          <input
            type="checkbox"
            class="osf-flag-box"
            value={opt}
            checked={selected.has(opt)}
            disabled={disabled}
            onChange={(e) => toggle(opt, (e.currentTarget as HTMLInputElement).checked)}
          />
          <span class="osf-flag-label">{optionLabel(setting, opt)}</span>
        </label>
      ))}
    </div>
  );
}
