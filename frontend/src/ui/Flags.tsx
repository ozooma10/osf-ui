// The `type:"flags"` multi-select (a checkbox group).
//
// Every commit is the whole array, built by filtering the declared options
// (`opts.filter((o) => selected.has(o))`) rather than by splicing the stored
// array. That mirrors what SettingsStore.cpp does: canonicalise to declared
// order, drop unknown options, drop non-string junk, dedupe. Splicing instead
// would preserve whatever order and garbage the file held, the store would
// canonicalise it, and the echo would then differ from the optimistic local
// value — which the settings App reads as an external writer and repaints the
// pane mid-edit. Rendering follows the same rule: iterate `options`, not the
// value.

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
  // Non-string entries are dropped inbound as well as outbound: a `["a", 3]`
  // value must not make option "3" look checked.
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
