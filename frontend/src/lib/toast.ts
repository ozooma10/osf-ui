
/** Milliseconds from insertion until the `leaving` class is applied. */
export const TOAST_LEAVING_MS = 2600;
/** Milliseconds from insertion until the entry is removed. */
export const TOAST_REMOVE_MS = 3000;

export type ToastKind = 'warn' | 'danger';

export interface ToastEntry {
  /** Monotonic within a state chain; the identity timers refer back to. */
  readonly id: number;
  readonly message: string;
  readonly kind?: ToastKind;
  /** True once TOAST_LEAVING_MS has elapsed — drives the `leaving` class. */
  readonly leaving: boolean;
}

export interface ToastState {
  readonly entries: readonly ToastEntry[];
  /** Next id to hand out. Never reused, so a stale timer can never hit a new entry. */
  readonly nextId: number;
}

export const initialToastState: ToastState = { entries: [], nextId: 1 };

/** One timer the caller must arm, relative to now. */
export interface ToastTimer {
  readonly id: number;
  readonly delayMs: number;
  readonly action: 'leaving' | 'remove';
}

export interface ToastAddResult {
  readonly state: ToastState;
  readonly entry: ToastEntry;
  /** Both timers, in firing order. Arm them independently. */
  readonly timers: readonly ToastTimer[];
}

export function addToast(state: ToastState, message: string, kind?: ToastKind): ToastAddResult {
  const id = state.nextId;
  const entry: ToastEntry = kind === undefined
    ? { id, message, leaving: false }
    : { id, message, kind, leaving: false };

  return {
    state: { entries: [...state.entries, entry], nextId: id + 1 },
    entry,
    timers: [
      { id, delayMs: TOAST_LEAVING_MS, action: 'leaving' },
      { id, delayMs: TOAST_REMOVE_MS, action: 'remove' },
    ],
  };
}

export function expireToast(state: ToastState, id: number): ToastState {
  const target = state.entries.find((e) => e.id === id);
  if (!target || target.leaving) return state;
  return {
    ...state,
    entries: state.entries.map((e) => (e.id === id ? { ...e, leaving: true } : e)),
  };
}

/** Remove a toast (the TOAST_REMOVE_MS timer). Unknown ids are ignored. */
export function removeToast(state: ToastState, id: number): ToastState {
  if (!state.entries.some((e) => e.id === id)) return state;
  return { ...state, entries: state.entries.filter((e) => e.id !== id) };
}

/** Class list for an entry: base `toast`, optional kind modifier, `leaving`. */
export function toastClassName(entry: ToastEntry): string {
  return `toast${entry.kind ? ` toast--${entry.kind}` : ''}${entry.leaving ? ' leaving' : ''}`;
}
