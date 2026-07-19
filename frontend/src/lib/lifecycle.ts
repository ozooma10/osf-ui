// lifecycle.ts — the overlay-visit reset and the LB/RB rail cycling, as pure
// state transitions.
//
// Both behaviours live in the settings view (main.legacy.js:1839-1889) and both
// are entangled with the DOM there. This module extracts the DECISIONS and
// leaves the effects to the caller: it never touches `document`, `window` or
// `padnav`, it just says what should happen.
//
// Why the split matters: the padnav reset is a genuine side effect on a global
// (`window.padnav.reset()`, main.legacy.js:1855) and the filter/selection reset
// is a state change. Returning the intent lets the reducer be tested without
// jsdom while the component stays a thin dispatcher.

import type { UiGamepadPayload, UiVisibilityPayload } from '@sdk';

// ---------------------------------------------------------------------------
// visibility
// ---------------------------------------------------------------------------

/**
 * The Home launcher's rail id (settings/main.legacy.js:35). "~" keeps it out of
 * the mod-id namespace — native mod ids never start with it — so it cannot
 * shadow a real entry.
 */
export const HOME_ID = '~home';

/** The slice of view state a visibility flip can touch. */
export interface LifecycleState {
  /** Currently selected rail entry id. */
  readonly selectedId: string;
  /** Current rail filter text (the `#filter` input's raw value). */
  readonly filter: string;
}

export interface VisibilityIntent {
  readonly state: LifecycleState;
  /**
   * Drop the undo baseline. The undo scope is "since you opened settings", not
   * the whole game session — the view keeps running while hidden, so without
   * this it accumulates every change ever made (main.legacy.js:1841-1846).
   */
  readonly clearBaseline: boolean;
  /**
   * Re-select Home and re-render. FALSE when the view was already sitting on
   * Home with an empty filter — the legacy code guards the whole reset on
   * `selectedId !== HOME_ID || filterEl.value` (main.legacy.js:1850), so the
   * no-op case genuinely skips the rail+detail rebuild. Preserved: rebuilding
   * unconditionally would tear down and re-create the pane on every show edge.
   */
  readonly reselect: boolean;
  /**
   * Call `padnav.reset()`. Note this is UNCONDITIONAL on the show edge — it
   * sits outside the `reselect` guard (main.legacy.js:1854-1855), so a visit
   * that lands back on an already-selected Home still forgets the gamepad
   * resume point.
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
 * Only the closed->open edge does anything; `visible: false` is ignored
 * entirely, which is why the view retains its selection while hidden and only
 * loses it on the next open.
 */
export function reduceVisibility(
  state: LifecycleState,
  payload: UiVisibilityPayload,
): VisibilityIntent {
  if (!payload.visible) return inert(state);

  // "Land every visit on the launcher, filter cleared — the toggle key means
  // 'open the deck', not 'resume where a past visit left off'."
  const needsReselect = state.selectedId !== HOME_ID || state.filter !== '';

  return {
    state: needsReselect ? { selectedId: HOME_ID, filter: '' } : state,
    clearBaseline: true,
    reselect: needsReselect,
    resetPadnav: true,
  };
}

// ---------------------------------------------------------------------------
// gamepad shoulder buttons -> rail cycling
// ---------------------------------------------------------------------------

/** XInput LB. Steps the rail selection backwards. */
export const PAD_LSHOULDER = 0x0100;
/** XInput RB. Steps the rail selection forwards. */
export const PAD_RSHOULDER = 0x0200;

/**
 * Which buttons are currently held. `ui.gamepad` is a raw firehose — a held
 * button can be reported `down: true` on every poll — so the reducer needs its
 * own memory to fire once per press.
 *
 * NOTE ON FIDELITY: the legacy handler (main.legacy.js:1866-1871) trusts
 * `p.button.down` alone and has no such memory; it would cycle the rail once
 * per repeat frame if the runtime ever re-reported a held button. In practice
 * the runtime only emits on transitions, so the observable behaviour is the
 * same — this state just makes the guarantee local instead of assumed.
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
 * Track press/release and report the DOWN edge only.
 *
 * A `down: false` report clears the memory so the next press registers again.
 * Stick events pass through untouched — this view never reads the axes (the
 * runtime's default mapping already turns the stick into arrow keys).
 */
export function padButtonEdge(state: PadButtonState, payload: UiGamepadPayload): PadEdge {
  // Defensive to the same depth as the legacy guard
  // (`!p || p.kind !== "button" || !p.button || ...`, main.legacy.js:1867).
  // The declared type says `button` is always present on a button payload, but
  // this is an untrusted native push crossing the bridge as JSON — the legacy
  // code checked, and dropping the check would turn a malformed frame from an
  // ignored event into a TypeError that kills the whole `ui.gamepad`
  // subscription for the rest of the visit.
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
   * the current filter. Built by the caller because it needs the live mod list;
   * see main.legacy.js:1875-1885.
   */
  readonly railIds: readonly string[];
  readonly selectedId: string;
  /**
   * True while the undo/revert panel is up. The modal owns input, so shoulder
   * presses must not move the rail underneath it (main.legacy.js:1869 tests
   * `document.querySelector(".session-overlay")`).
   */
  readonly modalOpen: boolean;
}

/**
 * The id `delta` steps to, wrapping — or the current id when nothing moves.
 *
 * Quirk preserved (main.legacy.js:1886-1887): when the current selection is not
 * in the list at all (`indexOf` -> -1) the result is `railIds[0]` REGARDLESS of
 * direction, so LB and RB both jump to Home/the first entry rather than to
 * opposite ends. That is the recovery path after a filter hid the selection.
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
 * Ordering note: the edge is consumed even when a modal is open, so releasing
 * and re-pressing after the modal closes still reads as a fresh press. The
 * legacy code has the same shape (its `down` test precedes the modal test).
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
