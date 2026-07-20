// Transient-notice stack. @lib/toast owns the state machine and timings; this
// file owns the DOM and the timers. The fade and removal delays are independent
// and both measured from insertion (see the header of src/lib/toast.ts), so
// removal must not be chained off the fade — that would desynchronise it from
// the CSS transition window.
//
// `aria-live="polite"` sits on the stack, not the entry, so the announcement
// fires on insertion — the same reason legacy kept a permanent container node.

import { useEffect, useRef, useState } from 'preact/hooks';
import {
  addToast,
  expireToast,
  initialToastState,
  removeToast,
  toastClassName,
  type ToastEntry,
  type ToastKind,
  type ToastState,
} from '@lib/toast';

export interface Toasts {
  entries: readonly ToastEntry[];
  /** Append a notice; newest renders last. */
  push: (message: string, kind?: ToastKind) => void;
}

/**
 * Owns a toast list plus its timers.
 *
 * State is mirrored into a ref because `push` is called from bridge callbacks
 * holding an older render's closure; reading the ref keeps the id counter
 * monotonic across those. Timer ids are tracked so an unmount mid-flight cannot
 * fire a setState into a torn-down tree.
 */
export function useToasts(): Toasts {
  const [state, setState] = useState<ToastState>(initialToastState);
  const stateRef = useRef<ToastState>(initialToastState);
  // `ReturnType<typeof setTimeout>`, not `number`: @types/node is in the
  // devDependency graph, so the ambient signature is Node's and returns Timeout.
  const timers = useRef<ReturnType<typeof setTimeout>[]>([]);

  useEffect(() => {
    const pending = timers.current;
    return () => {
      for (const t of pending) clearTimeout(t);
    };
  }, []);

  const apply = (next: ToastState) => {
    stateRef.current = next;
    setState(next);
  };

  const push = (message: string, kind?: ToastKind) => {
    // `addToast` is overloaded on the absence of `kind`. An explicit undefined
    // would also suppress the modifier class, but building the call
    // conditionally keeps `exactOptionalPropertyTypes` honest.
    const result = kind === undefined
      ? addToast(stateRef.current, message)
      : addToast(stateRef.current, message, kind);
    apply(result.state);
    for (const timer of result.timers) {
      timers.current.push(
        setTimeout(() => {
          apply(
            timer.action === 'leaving'
              ? expireToast(stateRef.current, timer.id)
              : removeToast(stateRef.current, timer.id),
          );
        }, timer.delayMs),
      );
    }
  };

  return { entries: state.entries, push };
}

export interface ToastStackProps {
  entries: readonly ToastEntry[];
  /** DOM id of the container; legacy used "toast". */
  id: string;
}

export function ToastStack({ entries, id }: ToastStackProps) {
  return (
    <div id={id} class="toast-stack" aria-live="polite">
      {entries.map((entry) => (
        <div key={entry.id} class={toastClassName(entry)}>
          {entry.message}
        </div>
      ))}
    </div>
  );
}
