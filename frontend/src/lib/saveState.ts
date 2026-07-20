// Write-behind save indicator.
//
// Native persistence is write-behind: `settings.changed` is the immediate
// in-memory commit, `settings.persisted` is the disk write landing ~0.5s later,
// coalesced per mod and guaranteed on menu close. The indicator shows "Saving…"
// from the moment a write is sent until every mod this view touched has
// confirmed, then a fading "Saved". Persisted pushes for writes this view did not
// make (a sibling DLL, another view's panel) are ignored — that is what the
// pending set is for.
//
// The three transitions are asymmetric on purpose; see each function.

/** Milliseconds the "Saved" state stays up before its classes are removed. */
export const SAVE_FADE_MS = 1800;

/**
 * Which string the indicator shows. A token, not translated text, so this module
 * stays pure — the component maps it through its translator.
 */
export type SaveLabel = 'saving' | 'saved';

export interface SaveState {
  /**
   * Mod ids with an in-flight write. The indicator is a function of this set
   * draining, not of any single write completing.
   */
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

/**
 * A write was sent for `modId` (settings.set, settings.reset, a preset entry).
 *
 * Unconditionally re-shows "Saving…", cancels a pending fade, and drops "done",
 * so a second write while a "Saved" fade is in flight reverts the indicator
 * instead of letting the stale fade win.
 */
export function saveStatePending(state: SaveState, modId: string): SaveTransition {
  const pending = new Set(state.pending);
  pending.add(modId);
  return {
    state: { pending, label: 'saving', classes: ['visible'] },
    cancelFade: true,
    scheduleFadeMs: null,
  };
}

/**
 * `settings.persisted` arrived for `modId` — its values file write landed.
 *
 * Two behaviours that look like bugs and are not:
 *
 *  1. It shows "Saved" only when the set drains to empty. With writes to other
 *     mods still outstanding the entry is removed but the indicator keeps saying
 *     "Saving…", because the visit is not fully persisted yet.
 *
 *  2. It does not clear the indicator — it swaps the text to "Saved" and adds
 *     "visible done"; both classes come off later via the 1800ms timer. Clearing
 *     is `saveStateAbandon`'s job. So there is a window where the indicator reads
 *     "Saved" while `pending` is empty.
 *
 * The set is mutated even on the early-return path: the delete runs before the
 * `size` short-circuit, so an unknown/late mod id is still consumed.
 */
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

/**
 * A write for `modId` was rejected (settings.set / settings.reset rejected).
 *
 * The one transition that clears the indicator: a rejected write never persists,
 * so "Saving…" would otherwise stick forever. If other changes to the same mod
 * are still pending the entry is gone, so its eventual persisted push finds
 * nothing — losing the confirmation, never showing a false one.
 *
 * Quirks: `label` is left as-is, so the hidden element still reads "Saving…"
 * underneath. And unlike the other two transitions this does not cancel an armed
 * fade timer — harmless, since that timer removes the classes just removed here.
 */
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

/** The fade timer firing: drop "visible" and "done" together. `label` is untouched. */
export function saveStateFaded(state: SaveState): SaveState {
  if (!state.classes.length) return state;
  return { ...state, classes: [] };
}

/** Is the indicator on screen? */
export function isSaveStateVisible(state: SaveState): boolean {
  return state.classes.includes('visible');
}
