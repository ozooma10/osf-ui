// A navigation band.
//
// `class="row"` is a contract with padnav.js, which measures cross-axis
// distance between the nearest `.row` ancestors rather than between the
// elements themselves — so everything inside one Row counts as a single
// navigation line. That is what lets a left-aligned group header and the
// right-aligned control beneath it read as vertically adjacent instead of as
// two different columns. Shared by settings and keybinds so they cannot drift.

import type { ComponentChildren } from 'preact';

export interface RowProps {
  children: ComponentChildren;
  /**
   * Classes appended after the mandatory `row`. Pass "" for none — an optional
   * prop would run into `exactOptionalPropertyTypes` at every call site.
   */
  class: string;
  /** `data-label`; "" omits the attribute. */
  dataLabel: string;
  /** `data-key`; "" omits the attribute. */
  dataKey: string;
}

export function Row({ children, class: extra, dataLabel, dataKey }: RowProps) {
  return (
    <div
      class={extra ? `row ${extra}` : 'row'}
      // Emitted only when non-empty: an empty `data-label=""` would match
      // `[data-label]` selectors that expect content.
      {...(dataLabel ? { 'data-label': dataLabel } : {})}
      {...(dataKey ? { 'data-key': dataKey } : {})}
    >
      {children}
    </div>
  );
}
