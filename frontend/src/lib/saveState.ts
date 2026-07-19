// saveState.ts — the write-behind save indicator.
//
// Native persistence is write-behind: `settings.changed` is the immediate
// in-memory commit, `settings.persisted` is the disk write landing ~0.5s later,
// coalesced per mod and guaranteed on menu close. The indicator shows "Saving…"
// from the moment a write is sent until EVERY mod this view touched has
// confirmed, then a fading "Saved". Persisted pushes for writes this view did
// not make (a sibling DLL, another view's panel) are deliberately ignored,
// which is what the pending set is for.
//
// Ported from settings/main.legacy.js:126-156. The three transitions are
// deliberately ASYMMETRIC and the asymmetry is the whole design — see each
// function.

/** Milliseconds the "Saved" state stays up before its classes are removed. */
export const SAVE_FADE_MS = 1800;

/**
 * Which string the indicator shows. Kept as a token, not translated text, so
 * this module stays pure — the component maps it through its translator
 * ("saving" -> tr("saving", "Saving…"), "saved" -> tr("saved", "Saved")).
 */
export type SaveLabel = 'saving' | 'saved';

export interface SaveState {
  /**
   * Mod ids with an in-flight write. The indicator is a function of this set
   * DRAINING, not of any single write completing.
   */
  readonly pending: ReadonlySet<string>;
  /**
   * Current label, or null before the first write. The legacy element starts
   * with empty text and only ever gains one of the two strings.
   */
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
 * A write was SENT for `modId` (settings.set, settings.reset, a preset entry).
 *
 * Unconditional: it always re-shows "Saving…", always cancels a pending fade,
 * and always drops "done". A second write while a "Saved" fade is in flight
 * therefore reverts the indicator to "Saving…" rather than letting the stale
 * fade win.
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
 *  1. It fires ONLY when the set drains to empty. With other writes to other
 *     mods still outstanding the entry is removed but the indicator keeps
 *     saying "Saving…" — correct, because the visit is not fully persisted yet.
 *     (`!pendingSaveMods.delete(modId) || pendingSaveMods.size > 0` returns
 *     early, legacy:144.)
 *
 *  2. It does NOT clear the indicator. It swaps the text to "Saved" and ADDS
 *     "visible done" — "visible" is already present, and both classes are only
 *     removed later by the 1800ms timer (legacy:145-148). Clearing here is
 *     `saveStateAbandon`'s job, not this one. So there is a window where the
 *     indicator is visibly "Saved" while `pending` is empty.
 *
 * The set is mutated even on the early-return path: `Set.delete` runs before
 * the `size` short-circuit, so an unknown/late mod id is still consumed.
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
 * A write for `modId` was REJECTED (settings.set / settings.reset rejected).
 *
 * The one transition that actually clears the indicator: a rejected write never
 * persists, so "Saving…" would otherwise stick forever. If OTHER changes to the
 * same mod are still pending the entry is simply gone, so its eventual
 * persisted push finds nothing — losing the confirmation, never showing a false
 * one (legacy:150-156).
 *
 * QUIRK PRESERVED: `label` is deliberately left as-is (the legacy code only
 * touches `classList`, never `textContent`), so the hidden element still reads
 * "Saving…" underneath. And unlike the other two transitions this does NOT
 * cancel an armed fade timer — harmless, because that timer removes exactly the
 * classes this just removed.
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

/**
 * The fade timer firing: drop "visible" and "done" together. `label` is again
 * untouched, matching `classList.remove("visible", "done")` (legacy:148).
 */
export function saveStateFaded(state: SaveState): SaveState {
  if (!state.classes.length) return state;
  return { ...state, classes: [] };
}

/** Convenience for the component: is the indicator on screen? */
export function isSaveStateVisible(state: SaveState): boolean {
  return state.classes.includes('visible');
}
