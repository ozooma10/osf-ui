// ColorField.tsx — the `type:"string"` colour widget (`widget:"color"`).
//
// Ports `buildColor` (settings/main.legacy.js:473-498): a swatch, a hex text
// field, and a strip of preset chips.
//
// TWO behaviours that look like bugs and are not:
//
//  1. AN INVALID HEX REVERTS TO THE LAST COMMITTED COLOUR, not to the value the
//     visit started with. Legacy tracked it in a closure variable (`current`,
//     reassigned on every successful commit at main.legacy.js:485 and :493) for
//     exactly this reason — typing junk after three good edits must not throw
//     away the three good edits. Here the local model IS the last committed
//     value (commits apply optimistically), so `value` plays that role.
//
//  2. THE REGEX ACCEPTS UPPERCASE. `HEX_RE` is `[0-9a-fA-F]`, so "#5AA9B8"
//     commits verbatim — it is NOT lowercased on the way in, and the store
//     accepts it too. Anything that "normalises" case here would make the
//     round-tripped echo differ from the optimistic value.
//
// The swatch always shows the COMMITTED colour, never what is being typed:
// legacy repaints it only from inside the change handler, on the value it just
// accepted (or on the value it just reverted to). So it derives from `value`.

import { useEffect, useRef, useState } from 'preact/hooks';
import { HEX_RE } from '@lib/settings/normalize';

/**
 * The chip strip (main.legacy.js:41). A curated in-house palette, not derived
 * from anything — kept in this exact order because the row is a fixed-width
 * grid and shuffling it moves every chip under the user's cursor.
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
  // The text field is edit-in-progress state: it must hold whatever the user is
  // typing, so it cannot be driven straight off the model. Legacy got this free
  // from an uncontrolled input.
  const [text, setText] = useState(committed);

  // Re-seed when the MODEL moves under us (an external writer, a preset, a
  // reset). Guarded on a change in `committed` rather than run unconditionally,
  // or every keystroke's re-render would wipe the field.
  const lastCommitted = useRef(committed);
  useEffect(() => {
    if (lastCommitted.current !== committed) {
      lastCommitted.current = committed;
      setText(committed);
    }
  }, [committed]);

  // Reads the field's LIVE value (not `text` state): legacy did
  // `hex.value.trim()` off the DOM node on change (main.legacy.js:482), so a
  // programmatic set-then-change — and the keyboard-nav commit path, which
  // blurs without a preceding input — both see the current text.
  const apply = (raw: string) => {
    const v = raw.trim();
    if (HEX_RE.test(v)) {
      // Keep the field in sync with what was committed (it may have been set
      // programmatically, leaving `text` state behind).
      setText(v);
      onCommit(v);
      return;
    }
    // Revert to the last committed colour and tell the caller to warn.
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
            {/* Empty: the chip IS the swatch. */}
          </button>
        ))}
      </div>
    </div>
  );
}
