// Transient-notice list, as a pure state machine.
//
// The two timers are independent and both measured from insertion — removal is
// not chained off the fade. The 400ms difference is the CSS transition window
// for the `.leaving` class; changing either number desynchronises the fade-out
// from the unmount, so they must move together with osfui.css.

/** Milliseconds from insertion until the `leaving` class is applied. */
export const TOAST_LEAVING_MS = 2600;
/** Milliseconds from insertion until the entry is removed. */
export const TOAST_REMOVE_MS = 3000;

/**
 * Rendered as `toast--<kind>` alongside the base `toast` class; an omitted kind
 * renders the base class alone (no `toast--undefined`), which is why this is
 * optional rather than defaulted.
 *
 * These are the only kinds the settings and keybinds stylesheets define (both
 * set only `border-left-color`). A new kind must land in both stylesheets
 * first, or it renders identically to a plain toast.
 */
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

/**
 * Append a toast. Newest goes last, so toasts stack downward in arrival order.
 * No cap and no de-duplication — a burst of rejected writes shows one toast
 * each.
 */
export function addToast(state: ToastState, message: string, kind?: ToastKind): ToastAddResult {
  const id = state.nextId;
  // Built conditionally: `exactOptionalPropertyTypes` forbids assigning an
  // explicit `undefined` to an optional property, and the absent/undefined
  // distinction is what suppresses the `toast--` modifier class.
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

/**
 * Mark a toast as leaving (the TOAST_LEAVING_MS timer).
 *
 * Unknown id is a no-op returning the same state reference, so a component
 * driven by identity does not re-render. An id-keyed model can race a removal,
 * so this tolerates a miss rather than throwing.
 */
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
