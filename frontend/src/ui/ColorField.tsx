
import { HEX_RE } from '@lib/settings/normalize';
import { useCommittedText } from './useCommittedText';

export const COLOR_PRESETS = [
  '#5aa9b8',
  '#6fae6a',
  '#e0a23c',
  '#c8503a',
  '#c8607f',
  '#f0ece2',
  '#828a93',
  '#11151b',
] as const;

export interface ColorFieldProps {
  id: string;
  value: string | undefined;
  disabled: boolean;
  onCommit: (next: string) => void;
  /** Fired instead of a commit when the typed text is not a hex colour. */
  onInvalid: () => void;
}

export function ColorField({ id, value, disabled, onCommit, onInvalid }: ColorFieldProps) {
  const committed = value || '';
  const [text, setText] = useCommittedText(committed);

  const apply = (raw: string) => {
    const v = raw.trim();
    if (HEX_RE.test(v)) {
      setText(v);
      onCommit(v);
      return;
    }
    setText(committed);
    onInvalid();
  };

  return (
    <div class="osf-color" id={id}>
      <span
        class="osf-color-swatch"
        style={{ background: HEX_RE.test(committed) ? committed : 'transparent' }}
      />
      <input
        type="text"
        class="osf-input osf-color-hex"
        value={text}
        spellcheck={false}
        // 9 = "#" + 8 hex digits (the #rrggbbaa form).
        maxLength={9}
        disabled={disabled}
        onInput={(e) => setText((e.currentTarget as HTMLInputElement).value)}
        onChangeCapture={(e) => apply((e.currentTarget as HTMLInputElement).value)}
      />
      <div class="osf-color-presets">
        {COLOR_PRESETS.map((p) => (
          <button
            key={p}
            type="button"
            class="osf-color-preset"
            style={{ background: p }}
            title={p}
            disabled={disabled}
            onClick={() => {
              setText(p);
              onCommit(p);
            }}
          >
            {/* Empty: the chip is the swatch. */}
          </button>
        ))}
      </div>
    </div>
  );
}
