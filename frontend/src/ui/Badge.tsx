// Inline status pill on a setting's label line: `osf-badge` (neutral input
// context), `--warn` (`requires`: restart / reload / new game), `--stop` (key
// binding conflict).
//
// The text is untrusted — `requiresLabel` echoes an unrecognised `requires`
// value back raw (see @lib/settings/format) and a hotkey context's `label` is
// schema author text — but renders as a text child, never as markup.

import type { ComponentChildren } from 'preact';
import { cx } from './cx';
import { optAttr } from './optAttr';

export interface BadgeProps {
  children: ComponentChildren;
  /**
   * Modifier appended after the base `osf-badge`, e.g. "osf-badge--warn".
   * Pass "" for the neutral badge — `cx` drops it (the kit's required-with-""
   * convention; the rationale is consolidated in optAttr's doc).
   */
  modifier: string;
  /** Tooltip. "" omits the attribute rather than emitting an empty one. */
  title: string;
}

export function Badge({ children, modifier, title }: BadgeProps) {
  return (
    <span class={cx('osf-badge', modifier)} {...optAttr('title', title)}>
      {children}
    </span>
  );
}
