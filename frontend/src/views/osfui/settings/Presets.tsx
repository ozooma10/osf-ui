
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
  if (!Array.isArray(presets) || !presets.length) return null;

  return (
    <div class="presets">
      <span class="osf-eyebrow">{tr('presets', 'Presets')}</span>
      <div class="presets-row">
        {presets.map((p, i) => {
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
