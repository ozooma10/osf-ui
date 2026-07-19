// TextField.tsx — the plain `type:"string"` control, single-line or textarea.
//
// Ports the non-colour half of `buildString` (settings/main.legacy.js:452-471).
//
// COMMITS ON `change`, NOT `input`: the value goes over the bridge, through
// native validation and into a write-behind disk flush, so committing per
// keystroke would send one round trip per character. `change` fires on blur (and
// on Enter for a single-line input), which is the one edit boundary the DOM
// gives us for free. padnav leans on this too — its Enter handling on a text
// entry does `blur(); focus();` specifically to fire this event
// (src/legacy/padnav.js:232-237).
//
// The maxLength cap is `Math.min(256, setting.maxLength || 256)`, straight from
// @lib/settings/normalize (MAX_STRING_LEN). The store hard-caps strings at 256
// today, so accepting more here would let the UI show text the store silently
// truncates. Raising it is a native change first — bump both in lockstep.

import { useEffect, useRef, useState } from 'preact/hooks';
import { MAX_STRING_LEN } from '@lib/settings/normalize';
import type { Setting } from '@sdk';

export type TextSource = Pick<Setting, 'widget' | 'maxLength'>;

/**
 * The effective cap. QUIRK PRESERVED from main.legacy.js:458: `||` means
 * `maxLength: 0` reads as "unset" and gets the full 256.
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
  // Edit-in-progress state, for the same reason as ColorField: a controlled
  // input driven straight off the model would fight the user's typing.
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
