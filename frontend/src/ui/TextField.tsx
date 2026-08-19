
import { MAX_STRING_LEN } from '@lib/settings/normalize';
import { useCommittedText } from './useCommittedText';
import type { Setting } from '@sdk';

export type TextSource = Pick<Setting, 'widget' | 'maxLength'>;

export function textCap(setting: TextSource): number {
  return Math.min(MAX_STRING_LEN, setting.maxLength || MAX_STRING_LEN);
}

export interface TextFieldProps {
  id: string;
  setting: TextSource;
  value: string | undefined;
  disabled: boolean;
  onCommit: (next: string) => void;
}

export function TextField({ id, setting, value, disabled, onCommit }: TextFieldProps) {
  const [text, setText] = useCommittedText(value ?? '');
  const maxLength = textCap(setting);

  if (setting.widget === 'textarea') {
    return (
      <textarea
        class="osf-input osf-textarea"
        id={id}
        rows={3}
        maxLength={maxLength}
        value={text}
        disabled={disabled}
        onInput={(e) => setText((e.currentTarget as HTMLTextAreaElement).value)}
        onChangeCapture={(e) => onCommit((e.currentTarget as HTMLTextAreaElement).value)}
      />
    );
  }

  return (
    <input
      type="text"
      class="osf-input"
      id={id}
      maxLength={maxLength}
      value={text}
      disabled={disabled}
      onInput={(e) => setText((e.currentTarget as HTMLInputElement).value)}
      onChangeCapture={(e) => onCommit((e.currentTarget as HTMLInputElement).value)}
    />
  );
}
