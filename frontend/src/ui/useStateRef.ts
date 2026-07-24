import { useRef, useState } from 'preact/hooks';

/**
 * State that is also mirrored into a ref.
 *
 * The mirror exists because the bridge subscriptions are registered ONCE, on
 * mount, and their closures would otherwise read the values from the first
 * render forever. Anything a long-lived closure needs to read must therefore be
 * available through a ref, not through the state binding it closed over.
 *
 * The setter writes the ref BEFORE the state — that ordering is the point. A
 * closure that runs later in the same tick (another bridge message, a queued
 * callback) reads the value that was just set rather than the pre-update one,
 * which is what makes back-to-back pushes compose correctly.
 *
 * Returns `[value, setValue, ref]`. Callers needing extra work on write wrap
 * this rather than re-implementing it: call `setValue` for the state+ref pair
 * and do their own bookkeeping around it.
 */
export function useStateRef<T>(initial: T) {
  const [value, setValue] = useState<T>(initial);
  const ref = useRef<T>(initial);
  const set = (next: T) => {
    ref.current = next;
    setValue(next);
  };
  return [value, set, ref] as const;
}

/**
 * A ref that always holds the latest render's value.
 *
 * The read-only sibling of {@link useStateRef}: use it when the value is
 * already owned elsewhere (derived by a memo, or state this component does not
 * set) but a long-lived closure still has to see the current one. Assigning
 * during render is deliberate — the ref must be current before any effect or
 * subscription callback runs.
 */
export function useLatest<T>(value: T) {
  const ref = useRef<T>(value);
  ref.current = value;
  return ref;
}
