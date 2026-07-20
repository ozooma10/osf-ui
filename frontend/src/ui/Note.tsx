// Note.tsx — a static rich-text callout (`type:"note"`).
//
// The style whitelist is a security control: `item.style` is untrusted schema
// author text landing in the class list. Unchecked interpolation would let a
// schema write `style: "info x\" onmouseover=…"` into the attribute, or borrow
// arbitrary kit classes (including the ones that position modals). Anything
// outside the enum falls back to "info".
//
// The body goes through the micro-markdown renderer in Inline.tsx, which emits
// only <strong>/<em>/<code>/<br> and text — no links, no raw HTML.

import { Inline } from './Inline';

/** The only three accepted values. */
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
  /**
   * `visibleWhen` said no. Adds `hidden-cond` (CSS `display:none`) rather than
   * unmounting; padnav skips zero-sized rects, so both are equivalent to it.
   */
  hiddenCond: boolean;
}

export function Note({ style, text, hiddenCond }: NoteProps) {
  return (
    <div
      class={
        hiddenCond
          ? `osf-note osf-note--${noteStyle(style)} hidden-cond`
          : `osf-note osf-note--${noteStyle(style)}`
      }
    >
      {/* A note with no text renders as an empty callout, not "undefined". */}
      <Inline text={text == null ? '' : text} />
    </div>
  );
}
