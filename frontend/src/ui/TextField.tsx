// TextField.tsx — the plain `type:"string"` control, single-line or textarea.
//
// Commits on `change`, not `input`: the value crosses the bridge into native
// validation and a write-behind disk flush, so per-keystroke commits would be one
// round trip per character. `change` fires on blur (and on Enter for a single-line
// input). padnav depends on this — its Enter handling on a text entry does
// `blur(); focus();` to fire the event.
//
// maxLength comes from MAX_STRING_LEN in @lib/settings/normalize. The store
// hard-caps strings at 256, so a larger cap here would show text the store
// silently truncates. Raising it is a native change first — bump both in lockstep.

import { useEffect, useRef, useState } from 'preact/hooks';
import { MAX_STRING_LEN } from '@lib/settings/normalize';
import type { Setting } from '@sdk';

export type TextSource = Pick<Setting, 'widget' | 'maxLength'>;

/**
 * The effective cap. `||` means `maxLength: 0` reads as "unset" and gets the
 * full MAX_STRING_LEN.
 */
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
  const committed = value ?? '';
  // Edit-in-progress state: a controlled input driven straight off the model
  // would fight the user's typing.
  const [text, setText] = useState(committed);

  const lastCommitted = useRef(committed);
  useEffect(() => {
    if (lastCommitted.current !== committed) {
      lastCommitted.current = committed;
      setText(committed);
    }
  }, [committed]);

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
        onChange={(e) => onCommit((e.currentTarget as HTMLTextAreaElement).value)}
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
      onChange={(e) => onCommit((e.currentTarget as HTMLInputElement).value)}
    />
  );
}
