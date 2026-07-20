// Micro-markdown renderer for `type:"note"` bodies. The whole grammar:
//
//   **bold**   -> <strong>
//   *italic*   -> <em>
//   `code`     -> <code>
//   \n         -> <br>
//
// No links, no raw HTML, no nesting. Note text is untrusted schema-author text;
// safety is structural — the only elements emitted are the four above and every
// other character lands as a text child, never innerHTML.
//
// The regex is rebuilt per line, not hoisted: it carries the `g` flag, so a
// shared instance would leak `lastIndex` between lines and drop matches.

import { Fragment } from 'preact';
import type { JSX } from 'preact';

/**
 * One alternation per emphasis form. Order matters: `**bold**` comes first so a
 * doubled asterisk is not mis-read as an empty italic. `[^*]+` / `[^`]+` stop a
 * marker spanning a nested marker, which keeps this non-recursive.
 */
const INLINE_RE = /(\*\*([^*]+)\*\*)|(\*([^*]+)\*)|(`([^`]+)`)/g;

/** Split one line into text nodes and emphasis elements. */
function renderLine(line: string): JSX.Element[] {
  const out: JSX.Element[] = [];
  const re = new RegExp(INLINE_RE.source, 'g');
  let last = 0;
  let m: RegExpExecArray | null;
  let n = 0;

  while ((m = re.exec(line)) !== null) {
    if (m.index > last) {
      out.push(<Fragment key={n++}>{line.slice(last, m.index)}</Fragment>);
    }
    // Capture groups mirror the alternation: 2 = bold, 4 = italic, 6 = code.
    // `!= null` rather than truthiness so an empty body still picks its branch.
    if (m[2] != null) out.push(<strong key={n++}>{m[2]}</strong>);
    else if (m[4] != null) out.push(<em key={n++}>{m[4]}</em>);
    else if (m[6] != null) out.push(<code key={n++}>{m[6]}</code>);
    last = re.lastIndex;
  }
  if (last < line.length) {
    out.push(<Fragment key={n++}>{line.slice(last)}</Fragment>);
  }
  return out;
}

export interface InlineProps {
  /** Untrusted schema text; coerced with String(). */
  text: unknown;
}

export function Inline({ text }: InlineProps) {
  const lines = String(text).split('\n');
  return (
    <>
      {lines.map((line, i) => (
        // Keyed by index: lines have no identity and the list is rebuilt on
        // every text change.
        <Fragment key={i}>
          {/* The <br> goes before each line but the first, so a trailing
              newline yields a trailing <br> and a leading one yields none. */}
          {i > 0 ? <br /> : null}
          {renderLine(line)}
        </Fragment>
      ))}
    </>
  );
}
