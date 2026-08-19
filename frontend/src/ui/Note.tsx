
import { Inline } from './Inline';
import { cx } from './cx';

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
  hiddenCond: boolean;
}

export function Note({ style, text, hiddenCond }: NoteProps) {
  const tone = noteStyle(style);
  return (
    <div
      class={cx('osf-note', tone === 'warn' && 'osf-note--warn', hiddenCond && 'hidden-cond')}
      style={tone === 'danger' ? { borderLeftColor: 'var(--osf-signal-stop)' } : undefined}
    >
      {/* A note with no text renders as an empty callout, not "undefined". */}
      <Inline text={text == null ? '' : text} />
    </div>
  );
}
