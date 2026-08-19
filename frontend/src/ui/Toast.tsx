
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

export function useToasts(): Toasts {
  const [state, setState] = useState<ToastState>(initialToastState);
  const stateRef = useRef<ToastState>(initialToastState);
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
