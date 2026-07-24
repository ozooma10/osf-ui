import { useEffect, useRef, useState } from 'preact/hooks';

/**
 * Edit-in-progress text state for a controlled field whose committed value can
 * also move underneath it.
 *
 * A field driven straight off the model would fight the user's typing, so the
 * live text is local state. It is re-seeded only when `committed` actually
 * changes (external writer, preset, reset) — the ref guard is the load-bearing
 * part: running the effect unconditionally would let every keystroke's re-render
 * wipe the field mid-edit.
 *
 * Callers compute their own `committed`, because the two differ on purpose:
 * TextField treats only null/undefined as empty (`value ?? ''`) while ColorField
 * also folds the empty string (`value || ''`).
 */
export function useCommittedText(committed: string) {
  const [text, setText] = useState(committed);

  const lastCommitted = useRef(committed);
  useEffect(() => {
    if (lastCommitted.current !== committed) {
      lastCommitted.current = committed;
      setText(committed);
    }
  }, [committed]);

  return [text, setText] as const;
}
