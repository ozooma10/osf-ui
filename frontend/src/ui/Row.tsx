// A navigation band.
//
// `class="row"` is a contract with padnav.js, which measures cross-axis
// distance between the nearest `.row` ancestors rather than between the
// elements themselves — so everything inside one Row counts as a single
// navigation line. That is what lets a left-aligned group header and the
// right-aligned control beneath it read as vertically adjacent instead of as
// two different columns.
//
// Used by the SETTINGS view only. Keybinds does not import this — its `HolderRow`
// builds its own `kb-holder` band and satisfies the padnav contract separately
// (see the dom-contracts test, which covers both). An earlier version of this
// comment claimed the two shared it "so they cannot drift"; they never did, and
// they can. Changing this file does not change keybinds.

import type { ComponentChildren } from 'preact';
import { cx } from './cx';
import { optAttr } from './optAttr';

export interface RowProps {
  children: ComponentChildren;
  /**
   * Classes appended after the mandatory `row`. Pass "" for none — `cx` drops it
   * (the kit's required-with-"" convention; rationale in optAttr's doc).
   */
  class: string;
  /** `data-key`; "" omits the attribute. The search-jump anchor. */
  dataKey: string;
}

export function Row({ children, class: extra, dataKey }: RowProps) {
  return (
    // data-key is emitted only when non-empty: an empty `data-key=""` would
    // match `[data-key]` selectors that expect content.
    <div class={cx('row', extra)} {...optAttr('data-key', dataKey)}>
      {children}
    </div>
  );
}
