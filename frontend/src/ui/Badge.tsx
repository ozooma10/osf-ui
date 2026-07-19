// Badge.tsx — the kit's inline status pill.
//
// One node, three flavours, all of which the settings view puts on a setting's
// label line (main.legacy.js:571-592):
//
//   `osf-badge`               the neutral input-context badge
//   `osf-badge osf-badge--warn`  a `requires` badge (restart / reload / new game)
//   `osf-badge osf-badge--stop`  a key-binding conflict
//
// The text is UNTRUSTED in two of those three cases — `requiresLabel` echoes an
// unrecognised `requires` value back raw (see @lib/settings/format), and an
// input context's `label` is schema author text. It is safe here for the same
// reason it was safe in legacy: this renders as a text child, never as markup.

import type { ComponentChildren } from 'preact';

export interface BadgeProps {
  children: ComponentChildren;
  /**
   * Modifier appended after the base `osf-badge`, e.g. "osf-badge--warn".
   * Pass "" for the neutral badge — an optional prop would trip
   * `exactOptionalPropertyTypes` at every call site for no gain.
   */
  modifier: string;
  /** Tooltip. "" omits the attribute entirely rather than emitting title="". */
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
