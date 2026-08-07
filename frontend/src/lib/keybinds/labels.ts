// labels.ts — localized keycap labels (the additive `keyboard` block of the
// settings data document).
//
// Names are the stable, layout-independent identity ("Semicolon" = a physical
// position); labels are what the player's layout prints there ("Ö"). The map
// is display-only and may be absent (older OSF UI runtime, browser preview) — every
// consumer falls back: chips fall back to the name, board cells to their
// authored US glyph.

import type { SettingsData } from '@sdk';

/**
 * Raw label lookup: the current layout's keycap for a canonical key name, or
 * undefined when the map has no (usable) entry — the caller picks its own
 * fallback.
 */
export type KeyLabeler = (name: string) => string | undefined;

const none: KeyLabeler = () => undefined;

/**
 * Build a labeler over `data.keyboard`. Hostile/absent shapes degrade to the
 * no-map labeler; lookups are own-property-guarded (mod-authored names like
 * "constructor" must not dredge up Object.prototype members) and only
 * non-empty strings count as labels.
 */
export function makeLabeler(keyboard: SettingsData['keyboard'] | null | undefined): KeyLabeler {
  const labels = keyboard && typeof keyboard === 'object' ? keyboard.labels : undefined;
  if (!labels || typeof labels !== 'object') return none;
  return (name) => {
    if (!Object.prototype.hasOwnProperty.call(labels, name)) return undefined;
    const label = (labels as Record<string, unknown>)[name];
    return typeof label === 'string' && label ? label : undefined;
  };
}
