
import { cx } from './cx';

export interface KeyFieldProps {
  id: string;
  /** The bound key name, or "" / undefined when unbound. */
  value: string | undefined;
  label?: string | undefined;
  allowUnbound: boolean;
  /** True while this field's capture is armed. */
  listening: boolean;
  disabled: boolean;
  onRebind: () => void;
  /** Commit "" (the unbound state). */
  onUnbind: () => void;
  /** Label shown while armed, e.g. tr("pressKey", "Press a key…"). */
  listeningLabel: string;
  /** `title` on the ✕. */
  unbindTitle: string;
  /** `aria-label` on the ✕ — names the setting, since "×" alone says nothing. */
  unbindLabel: string;
}

export function KeyField(props: KeyFieldProps) {
  const { id, value, allowUnbound, listening, disabled } = props;

  const button = (
    <button
      type="button"
      class={cx('osf-btn', 'osf-btn--sm', 'osf-key', listening && 'listening')}
      id={id}
      disabled={disabled}
      onClick={props.onRebind}
    >
      {/* Em-dash placeholder for "unbound", so an empty-string value shows the
          dash rather than an empty button. The localized keycap wins over the
          raw name when the OSF UI runtime published one. */}
      {listening ? props.listeningLabel : props.label || value || '—'}
    </button>
  );

  if (!allowUnbound || !value) return button;

  return (
    <span class="osf-key-wrap">
      {button}
      <button
        type="button"
        class="osf-btn osf-btn--sm osf-btn--ghost osf-key-clear"
        title={props.unbindTitle}
        aria-label={props.unbindLabel}
        disabled={disabled}
        onClick={props.onUnbind}
      >
        {/* U+00D7 MULTIPLICATION SIGN, the glyph the shipped view draws. */}
        ×
      </button>
    </span>
  );
}
