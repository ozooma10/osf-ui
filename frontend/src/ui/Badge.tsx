// Inline status pill on a setting's label line: `osf-badge` (neutral input
// context), `--warn` (`requires`: restart / reload / new game), `--stop` (key
// binding conflict).
//
// The text is untrusted — `requiresLabel` echoes an unrecognised `requires`
// value back raw (see @lib/settings/format) and an input context's `label` is
// schema author text — but renders as a text child, never as markup.

import type { ComponentChildren } from 'preact';

export interface BadgeProps {
  children: ComponentChildren;
  /**
   * Modifier appended after the base `osf-badge`, e.g. "osf-badge--warn".
   * Pass "" for the neutral badge; an optional prop would trip
   * `exactOptionalPropertyTypes` at every call site.
   */
  modifier: string;
  /** Tooltip. "" omits the attribute rather than emitting an empty one. */
  title: string;
}

export function Badge({ children, modifier, title }: BadgeProps) {
  return (
    <span
      class={modifier ? `osf-badge ${modifier}` : 'osf-badge'}
      {...(title ? { title } : {})}
    >
      {children}
    </span>
  );
}
