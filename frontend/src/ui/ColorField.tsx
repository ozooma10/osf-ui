// The `type:"string"` colour widget (`widget:"color"`): swatch, hex text field,
// preset chips.
//
// Two behaviours that look like bugs and are not:
//
//  1. An invalid hex reverts to the last committed colour, not to the value the
//     visit started with — typing junk after three good edits must not throw
//     away those edits. Commits apply optimistically, so `value` is the last
//     committed colour.
//
//  2. `HEX_RE` is `[0-9a-fA-F]`, so "#5AA9B8" commits verbatim; case is not
//     normalised on the way in and the store accepts it too. Lowercasing here
//     would make the round-tripped echo differ from the optimistic value.
//
// The swatch derives from `value`: it shows the committed colour, never what is
// being typed.

import { useEffect, useRef, useState } from 'preact/hooks';
import { HEX_RE } from '@lib/settings/normalize';

/**
 * Curated in-house palette, not derived from anything. Order is fixed: the row
 * is a fixed-width grid, so reshuffling moves every chip under the cursor.
 */
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
  // Edit-in-progress state: holds whatever the user is typing, so it cannot be
  // driven straight off the model.
  const [text, setText] = useState(committed);

  // Re-seed when the model moves under us (external writer, preset, reset).
  // Guarded on a change in `committed`; running it unconditionally would let
  // every keystroke's re-render wipe the field.
  const lastCommitted = useRef(committed);
  useEffect(() => {
    if (lastCommitted.current !== committed) {
      lastCommitted.current = committed;
      setText(committed);
    }
  }, [committed]);

  // Callers pass the field's live DOM value, not `text` state, so a programmatic
  // set-then-change and the keyboard-nav commit path (blur with no preceding
  // input) both see the current text.
  const apply = (raw: string) => {
    const v = raw.trim();
    if (HEX_RE.test(v)) {
      // Resync the field: it may have been set programmatically, leaving `text`
      // behind.
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
        onChange={(e) => apply((e.currentTarget as HTMLInputElement).value)}
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
