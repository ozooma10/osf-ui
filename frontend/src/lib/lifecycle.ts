// Overlay-visit reset and LB/RB rail cycling as pure state transitions. Touches
// neither `document`, `window` nor `padnav` — it returns the intent and the
// caller applies the effects.

import type { UiGamepadPayload, UiVisibilityPayload } from '@sdk';

/**
 * The Home launcher's rail id. "~" keeps it out of the mod-id namespace — native
 * mod ids never start with it — so it cannot shadow a real entry.
 */
export const HOME_ID = '~home';

/** The slice of view state a visibility flip can touch. */
export interface LifecycleState {
  /** Currently selected rail entry id. */
  readonly selectedId: string;
  /** Raw value of the `#filter` input. */
  readonly filter: string;
}

export interface VisibilityIntent {
  readonly state: LifecycleState;
  /**
   * Drop the undo baseline. Undo scope is "since you opened settings", not the
   * whole game session — the view keeps running while hidden, so without this it
   * accumulates every change ever made.
   */
  readonly clearBaseline: boolean;
  /**
   * Re-select Home and re-render. False when already sitting on Home with an
   * empty filter, so the no-op case skips the rail+detail rebuild instead of
   * tearing down and re-creating the pane on every show edge.
   */
  readonly reselect: boolean;
  /**
   * Call `padnav.reset()`. Unconditional on the show edge — it sits outside the
   * `reselect` guard, so a visit landing back on an already-selected Home still
   * forgets the gamepad resume point.
   */
  readonly resetPadnav: boolean;
}

/** The identity result: a hide edge changes nothing at all. */
function inert(state: LifecycleState): VisibilityIntent {
  return { state, clearBaseline: false, reselect: false, resetPadnav: false };
}

/**
 * Reduce a `ui.visibility` push.
 *
 * Only the closed->open edge does anything; `visible: false` is ignored, so the
 * view keeps its selection while hidden and loses it on the next open. A
 * `reason: 'focus'` edge is a focus switch within one visit, not a new visit,
 * so it resets nothing.
 */
export function reduceVisibility(
  state: LifecycleState,
  payload: UiVisibilityPayload,
): VisibilityIntent {
  if (!payload.visible || payload.reason === 'focus') return inert(state);

  // Every visit lands on the launcher with the filter cleared: the toggle key
  // means "open the deck", not "resume where a past visit left off".
  const needsReselect = state.selectedId !== HOME_ID || state.filter !== '';

  return {
    state: needsReselect ? { selectedId: HOME_ID, filter: '' } : state,
    clearBaseline: true,
    reselect: needsReselect,
    resetPadnav: true,
  };
}

/** XInput LB. Steps the rail selection backwards. */
export const PAD_LSHOULDER = 0x0100;
/** XInput RB. Steps the rail selection forwards. */
export const PAD_RSHOULDER = 0x0200;

/**
 * Which buttons are currently held. `ui.gamepad` is a raw firehose — a held
 * button can be reported `down: true` on every poll — so the reducer keeps its
 * own memory to fire once per press. The runtime only emits on transitions
 * today; this makes the once-per-press guarantee local rather than assumed.
 */
export interface PadButtonState {
  readonly down: readonly number[];
}

export const initialPadButtonState: PadButtonState = { down: [] };

export interface PadEdge {
  readonly state: PadButtonState;
  /** The button id that just went down, or null (no edge / not a button event). */
  readonly pressed: number | null;
}

/**
 * Track press/release and report the down edge only.
 *
 * A `down: false` report clears the memory so the next press registers again.
 * Stick events pass through untouched — this view does not read the axes; the
 * runtime's default mapping already turns the stick into arrow keys.
 */
export function padButtonEdge(state: PadButtonState, payload: UiGamepadPayload): PadEdge {
  // The declared type says `button` is always present on a button payload, but
  // this is an untrusted native push crossing the bridge as JSON. Dropping the
  // guard would turn a malformed frame from an ignored event into a TypeError
  // that kills the whole `ui.gamepad` subscription for the rest of the visit.
  if (!payload || payload.kind !== 'button' || !payload.button) return { state, pressed: null };
  const { id, down } = payload.button;
  const held = state.down.includes(id);

  if (!down) {
    if (!held) return { state, pressed: null };
    return { state: { down: state.down.filter((b) => b !== id) }, pressed: null };
  }
  // Already held: a repeat, not an edge. Report nothing but keep the memory.
  if (held) return { state, pressed: null };
  return { state: { down: [...state.down, id] }, pressed: id };
}

export interface RailCycleContext {
  /**
   * Rail entry ids in the exact order `renderRail` paints them: Home (only when
   * no filter is active), the framework, then title-sorted mods — all scoped by
   * the current filter. Built by the caller because it needs the live mod list.
   */
  readonly railIds: readonly string[];
  readonly selectedId: string;
  /**
   * True while the undo/revert panel is up (a `.session-overlay` element). The
   * modal owns input, so shoulder presses must not move the rail underneath it.
   */
  readonly modalOpen: boolean;
}

/**
 * The id `delta` steps to, wrapping — or the current id when nothing moves.
 *
 * Quirk: when the current selection is not in the list (`indexOf` -> -1) the
 * result is `railIds[0]` whatever the direction, so LB and RB both jump to the
 * first entry rather than to opposite ends. That is the recovery path after a
 * filter hid the selection.
 */
export function cycleRail(railIds: readonly string[], selectedId: string, delta: number): string {
  if (!railIds.length) return selectedId;
  const i = railIds.indexOf(selectedId);
  // Non-null assertion is safe: the index is taken modulo a non-empty length.
  return i < 0
    ? (railIds[0] as string)
    : (railIds[(i + delta + railIds.length) % railIds.length] as string);
}

export interface GamepadIntent {
  readonly state: PadButtonState;
  /** The rail entry to select, or null when nothing should change. */
  readonly select: string | null;
}

/**
 * Reduce a `ui.gamepad` push into (new edge state, rail selection to apply).
 *
 * The edge is consumed even when a modal is open, so releasing and re-pressing
 * after the modal closes still reads as a fresh press.
 */
export function reduceGamepad(
  state: PadButtonState,
  payload: UiGamepadPayload,
  ctx: RailCycleContext,
): GamepadIntent {
  const edge = padButtonEdge(state, payload);
  if (edge.pressed !== PAD_LSHOULDER && edge.pressed !== PAD_RSHOULDER) {
    return { state: edge.state, select: null };
  }
  if (ctx.modalOpen) return { state: edge.state, select: null };

  const next = cycleRail(ctx.railIds, ctx.selectedId, edge.pressed === PAD_LSHOULDER ? -1 : 1);
  // `selectMod` is only called when the id actually changes (a single-entry
  // rail must not re-render on every shoulder tap).
  return { state: edge.state, select: next === ctx.selectedId ? null : next };
}
