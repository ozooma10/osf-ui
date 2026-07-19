// Note.tsx — a static rich-text callout (`type:"note"`).
//
// Ports `buildNote` (settings/main.legacy.js:624-632).
//
// THE STYLE IS WHITELISTED, and that is a security control rather than a
// nicety: `item.style` is untrusted schema author text that lands in the class
// LIST. Interpolating it unchecked would let a schema write
// `style: "info x\" onmouseover=…"`-shaped values into the attribute, or at
// minimum borrow arbitrary classes from the kit's stylesheet (including the
// ones that position modals). Anything outside the enum falls back to "info".
//
// The body goes through the micro-markdown renderer, which can only emit
// <strong>/<em>/<code>/<br> and text — no links, no raw HTML. See Inline.tsx.

import { Inline } from './Inline';

/** main.legacy.js:627. The ONLY three accepted values. */
export const NOTE_STYLES = ['info', 'warn', 'danger'] as const;
export type NoteStyle = (typeof NOTE_STYLES)[number];

/** Coerce untrusted `style` to a known modifier. */
export function noteStyle(style: unknown): NoteStyle {
  return (NOTE_STYLES as readonly unknown[]).includes(style) ? (style as NoteStyle) : 'info';
}

export interface NoteProps {
  /** Untrusted. Anything not in NOTE_STYLES becomes "info". */
  style: unknown;
  /** Untrusted. Rendered through the micro-markdown grammar. */
  text: unknown;
}

export function Note({ style, text }: NoteProps) {
  return (
    <div class={`osf-note osf-note--${noteStyle(style)}`}>
      {/* `item.text || ""` — a note with no text renders as an empty callout
          rather than the string "undefined" (main.legacy.js:629). */}
      <Inline text={text == null ? '' : text} />
    </div>
  );
}
