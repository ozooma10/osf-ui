// Presets.tsx — the author-shipped value sets bar. Drawing only; the App's
// `applyPreset` commits the settings and toasts.
//
// A preset is a batch of ordinary writes, not a native operation: each key goes
// out as its own validated `settings.set`, so the store clamps and refuses per
// key as it would for a hand edit. A preset naming a key the mod does not have
// produces one rejected write rather than failing the whole batch.

import type { SettingsSchema } from '@sdk';
import type { Translator } from '@lib/i18n';

/** One entry; shape is not trusted, see the guard below. */
export type PresetRecord = NonNullable<SettingsSchema['presets']>[number];

export interface PresetsProps {
  presets: SettingsSchema['presets'];
  tr: Translator;
  onApply: (preset: PresetRecord) => void;
}

export function Presets({ presets, tr, onApply }: PresetsProps) {
  // Suppressed only when the array is missing or empty. A non-empty array whose
  // entries all fail the shape test below still paints the bar with an empty
  // row: the guard runs before the filtering loop.
  if (!Array.isArray(presets) || !presets.length) return null;

  return (
    <div class="presets">
      <span class="osf-eyebrow">{tr('presets', 'Presets')}</span>
      <div class="presets-row">
        {presets.map((p, i) => {
          // A preset with no value bag has nothing to apply, and firing it
          // would toast "Applied (0 settings)". This admits `values: null`
          // (typeof null === "object"); `applyPreset`'s `for...in` over null is
          // a no-op.
          if (!p || typeof p.values !== 'object') return null;
          return (
            <button
              key={p.id || p.label || i}
              type="button"
              class="osf-btn osf-btn--sm osf-btn--ghost"
              {...(p.description ? { title: p.description } : {})}
              onClick={() => onApply(p)}
            >
              {p.label || tr('preset', 'Preset')}
            </button>
          );
        })}
      </div>
    </div>
  );
}
