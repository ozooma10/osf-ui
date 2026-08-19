
/** Milliseconds the "Saved" state stays up before its classes are removed. */
export const SAVE_FADE_MS = 1800;

export type SaveLabel = 'saving' | 'saved';

export interface SaveState {
  readonly pending: ReadonlySet<string>;
  /** Current label, or null before the first write. */
  readonly label: SaveLabel | null;
  /** Classes on the indicator element: "visible" and/or "done". */
  readonly classes: readonly string[];
}

export interface SaveTransition {
  readonly state: SaveState;
  /** Cancel any armed fade timer. */
  readonly cancelFade: boolean;
  /** Arm a fade timer for this many ms, or null to arm nothing. */
  readonly scheduleFadeMs: number | null;
}

export const initialSaveState: SaveState = {
  pending: new Set<string>(),
  label: null,
  classes: [],
};

function withPending(state: SaveState, pending: ReadonlySet<string>): SaveState {
  return { ...state, pending };
}

export function saveStatePending(state: SaveState, modId: string): SaveTransition {
  const pending = new Set(state.pending);
  pending.add(modId);
  return {
    state: { pending, label: 'saving', classes: ['visible'] },
    cancelFade: true,
    scheduleFadeMs: null,
  };
}

export function saveStatePersisted(state: SaveState, modId: string): SaveTransition {
  const pending = new Set(state.pending);
  const wasPending = pending.delete(modId);

  if (!wasPending || pending.size > 0) {
    // No visual change — only the bookkeeping moves.
    return { state: withPending(state, pending), cancelFade: false, scheduleFadeMs: null };
  }

  return {
    state: { pending, label: 'saved', classes: ['visible', 'done'] },
    cancelFade: true,
    scheduleFadeMs: SAVE_FADE_MS,
  };
}

export function saveStateAbandon(state: SaveState, modId: string): SaveTransition {
  const pending = new Set(state.pending);
  const wasPending = pending.delete(modId);

  if (!wasPending || pending.size > 0) {
    return { state: withPending(state, pending), cancelFade: false, scheduleFadeMs: null };
  }

  return {
    state: { ...state, pending, classes: [] },
    cancelFade: false,
    scheduleFadeMs: null,
  };
}

export function saveStateFaded(state: SaveState): SaveState {
  if (!state.classes.length) return state;
  return { ...state, classes: [] };
}
