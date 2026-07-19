// Row.tsx — a navigation BAND.
//
// `class="row"` is not decoration: src/legacy/padnav.js:99-102 measures
// cross-axis distance between the nearest `.row` ancestors rather than between
// the elements themselves, so everything inside one Row counts as a single
// navigation line. That is what lets a left-aligned group header and the
// right-aligned control of the full-width row beneath it read as vertically
// adjacent instead of as two different columns.
//
// padnav ships verbatim and finds this by selector, so the class name is a
// contract with a file this migration is not allowed to touch. Wrapping it in a
// component means there is exactly one place that can get it wrong.
//
// (The settings view is the heavy consumer — settings/main.legacy.js:544-562
// builds one `.row` per setting. Kept here rather than under views/ so the
// keybinds and settings ports cannot drift apart on it.)

import type { ComponentChildren } from 'preact';

export interface RowProps {
  children: ComponentChildren;
  /**
   * Classes appended AFTER the mandatory `row`, matching the legacy build order
   * (`el("div", "row")` then `classList.add(...)`). Pass "" for none — an
   * optional prop would run into `exactOptionalPropertyTypes` at every call
   * site for no benefit.
   */
  class: string;
  /** Mirrors `row.dataset.label` (settings/main.legacy.js:561) — "" omits it. */
  dataLabel: string;
  /** Mirrors `row.dataset.key` (settings/main.legacy.js:562) — "" omits it. */
  dataKey: string;
}

export function Row({ children, class: extra, dataLabel, dataKey }: RowProps) {
  return (
    <div
      class={extra ? `row ${extra}` : 'row'}
      // Emitted only when non-empty: legacy assigned the dataset entries
      // unconditionally but always with a real value, and an empty
      // `data-label=""` would match `[data-label]` selectors that expect content.
      {...(dataLabel ? { 'data-label': dataLabel } : {})}
      {...(dataKey ? { 'data-key': dataKey } : {})}
    >
      {children}
    </div>
  );
}
