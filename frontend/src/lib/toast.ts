// toast.ts — the transient-notice list, as a pure state machine.
//
// Both views ship the same eight-line toast (settings/main.legacy.js:218-225,
// keybinds/main.legacy.js:55-61):
//
//   const t = el("div", "toast" + (kind ? " toast--" + kind : ""), message);
//   toastEl.appendChild(t);
//   setTimeout(() => { t.classList.add("leaving"); }, 2600);
//   setTimeout(() => { t.remove(); }, 3000);
//
// The two timers are INDEPENDENT and both measured from insertion — the removal
// is not chained off the fade. The 400ms difference is the CSS transition
// window for the `.leaving` class; changing either number desynchronises the
// fade-out from the unmount, so they are exported as named constants and must
// move together with osfui.css if they ever move at all.

/** Milliseconds from insertion until the `leaving` class is applied. */
export const TOAST_LEAVING_MS = 2600;
/** Milliseconds from insertion until the entry is removed. */
export const TOAST_REMOVE_MS = 3000;

/**
 * The visual variants the views use. Rendered as `toast--<kind>` alongside the
 * base `toast` class; an omitted kind renders the base class ALONE (no
 * `toast--undefined`), which is why this is optional rather than defaulted.
 *
 * EXACTLY TWO, deliberately. These are the only kinds any legacy call site
 * passes and the only ones the stylesheets define
 * (settings/style.css:272-273, keybinds/style.css:171-172, both of which set
 * only `border-left-color`). An "info" kind was tempting to add for symmetry
 * but would emit `toast--info`, which no stylesheet matches — it would render
 * identically to a plain toast while reading as intentional at the call site.
 * A new kind must land in both stylesheets first.
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
  /** Both timers, in firing order. Arm them independently — see the header. */
  readonly timers: readonly ToastTimer[];
}

/**
 * Append a toast. Newest goes LAST: the legacy code `appendChild`s, so toasts
 * stack downward in arrival order and CSS does the rest. There is no cap and no
 * de-duplication — a burst of rejected writes really does show one toast each.
 */
export function addToast(state: ToastState, message: string, kind?: ToastKind): ToastAddResult {
  const id = state.nextId;
  // Built conditionally: `exactOptionalPropertyTypes` forbids assigning an
  // explicit `undefined` to an optional property, and the absent/undefined
  // distinction is exactly what suppresses the `toast--` modifier class.
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
 * A no-op for an unknown id, and the state is returned UNCHANGED (same
 * reference) so a component driven by identity does not re-render. The legacy
 * equivalent cannot miss — it holds the node directly — but an id-keyed model
 * can race a removal, so this must be tolerant rather than throw.
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

/**
 * The class list for an entry, matching the legacy string concatenation exactly
 * (`"toast" + (kind ? " toast--" + kind : "")`, plus `leaving` added later).
 */
export function toastClassName(entry: ToastEntry): string {
  return `toast${entry.kind ? ` toast--${entry.kind}` : ''}${entry.leaving ? ' leaving' : ''}`;
}
