// Presets.tsx — the author-shipped value sets bar.
//
// Ports `renderPresets` (settings/main.legacy.js:952-967). Applying one is the
// App's job (`applyPreset`) because it commits N settings and toasts; this file
// only draws the bar.
//
// A preset is a BATCH OF ORDINARY WRITES, not a special native operation: each
// key goes out as its own validated `settings.set`, so the store clamps and
// refuses per key exactly as it would for a hand edit. That is why a preset
// naming a key the mod does not have simply produces one rejected write rather
// than failing the whole batch.

import type { SettingsSchema } from '@sdk';
import type { Translator } from '@lib/i18n';

/** One entry, defended the way the legacy loop defends it. */
export type PresetRecord = NonNullable<SettingsSchema['presets']>[number];

export interface PresetsProps {
  presets: SettingsSchema['presets'];
  tr: Translator;
  onApply: (preset: PresetRecord) => void;
}

export function Presets({ presets, tr, onApply }: PresetsProps) {
  // The bar is suppressed only when the ARRAY is missing or empty. A non-empty
  // array of entries that all fail the shape test below still paints the bar
  // with an empty row — faithful to main.legacy.js:953, where the guard runs
  // before the filtering loop.
  if (!Array.isArray(presets) || !presets.length) return null;

  return (
    <div class="presets">
      <span class="osf-eyebrow">{tr('presets', 'Presets')}</span>
      <div class="presets-row">
        {presets.map((p, i) => {
          // `typeof p.values !== "object"` — a preset with no value bag has
          // nothing to apply, and firing it would toast "Applied (0 settings)".
          // Note this admits `values: null` (typeof null === "object"), exactly
          // as legacy did; `applyPreset`'s `for...in` over null is a no-op.
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
