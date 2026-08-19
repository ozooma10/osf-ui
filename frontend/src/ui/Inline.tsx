
import { Fragment } from 'preact';
import type { JSX } from 'preact';

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
