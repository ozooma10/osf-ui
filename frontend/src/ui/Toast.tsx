// Toast.tsx — the transient-notice stack, driven by @lib/toast.
//
// @lib/toast owns the state machine and the two timings; this file owns only
// the DOM and the timers. The split matters because the two delays are
// independent and both measured from insertion (see the header of
// src/lib/toast.ts) — chaining the removal off the fade here would
// desynchronise it from the CSS transition window.
//
// Markup matches keybinds/main.legacy.js:55-61 and its index.html host node:
//   <div id="toast" class="toast-stack" aria-live="polite">
//     <div class="toast toast--warn">…</div>
//
// `aria-live="polite"` is on the STACK, not the entry, so an insertion is what
// gets announced — the same reason legacy kept a permanent container node.

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
  /** Append a notice. Newest renders last — see addToast(). */
  push: (message: string, kind?: ToastKind) => void;
}

/**
 * Own a toast list plus its timers.
 *
 * The state is mirrored into a ref because `push` is called from bridge
 * callbacks that captured an older render's closure; reading the ref keeps the
 * id counter monotonic across those. Timer ids are tracked so a view unmount
 * mid-flight cannot fire a setState into a torn-down tree.
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
    // `addToast` is overloaded on the ABSENCE of `kind` (an explicit undefined
    // would still suppress the modifier class, but building the call
    // conditionally keeps `exactOptionalPropertyTypes` honest about it).
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
