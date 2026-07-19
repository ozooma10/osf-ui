// Inline.tsx — the micro-markdown renderer for `type:"note"` bodies.
//
// Ports `renderInline` (settings/main.legacy.js:229-246), which built a
// DocumentFragment by hand. The grammar is deliberately TINY:
//
//   **bold**   -> <strong>
//   *italic*   -> <em>
//   `code`     -> <code>
//   \n         -> <br>
//
// and nothing else. No links, no raw HTML, no nesting. Note text is UNTRUSTED
// schema author text, and the legacy implementation's whole safety argument was
// that every literal went through `createTextNode` — never `innerHTML`. JSX
// gives us the same guarantee structurally: the only elements this can emit are
// the three above plus <br>, and every other character lands as a text child.
//
// The regex is rebuilt per line rather than hoisted to module scope on purpose:
// it carries the `g` flag, so a shared instance would leak `lastIndex` between
// lines (and between concurrent renders) and silently drop matches.

import { Fragment } from 'preact';
import type { JSX } from 'preact';

/**
 * One alternation per emphasis form, in the legacy order. Order matters: the
 * `**bold**` branch is FIRST, so a doubled asterisk is never mis-read as an
 * empty italic. `[^*]+` / `[^`]+` mean a marker cannot span a nested marker,
 * which is what keeps this non-recursive.
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
    // The capture groups are positional and mirror the alternation above:
    // 2 = bold body, 4 = italic body, 6 = code body. `!= null` rather than a
    // truthiness test so an (impossible, given `+`) empty body would still pick
    // the right branch.
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
  /** Untrusted schema text. Coerced with String() exactly as legacy did. */
  text: unknown;
}

export function Inline({ text }: InlineProps) {
  const lines = String(text).split('\n');
  return (
    <>
      {lines.map((line, i) => (
        // Keyed by index: lines have no identity of their own, and the whole
        // list is rebuilt whenever the text changes anyway.
        <Fragment key={i}>
          {/* The <br> goes BEFORE each line but the first — legacy appended it
              at the top of the loop body (main.legacy.js:233), so a trailing
              newline yields a trailing <br> and a leading one yields none. */}
          {i > 0 ? <br /> : null}
          {renderLine(line)}
        </Fragment>
      ))}
    </>
  );
}
