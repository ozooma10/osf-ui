
import type { UiGamepadPayload, UiVisibilityPayload } from '@sdk';

/** Re-exported so existing `@lib/lifecycle` consumers need no import change. */
export { HOME_ID } from './ids';
import { HOME_ID } from './ids';

/** The slice of view state a visibility flip can touch. */
export interface LifecycleState {
  /** Currently selected rail entry id. */
  readonly selectedId: string;
  /** Raw value of the `#filter` input. */
  readonly filter: string;
}

export interface VisibilityIntent {
  readonly state: LifecycleState;
  readonly clearBaseline: boolean;
  readonly reselect: boolean;
  readonly resetPadnav: boolean;
}

/** The identity result: a hide edge changes nothing at all. */
function inert(state: LifecycleState): VisibilityIntent {
  return { state, clearBaseline: false, reselect: false, resetPadnav: false };
}

export function reduceVisibility(
  state: LifecycleState,
  payload: UiVisibilityPayload,
): VisibilityIntent {
  if (!payload.visible || payload.reason === 'focus') return inert(state);

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

export interface PadButtonState {
  readonly down: readonly number[];
}

export const initialPadButtonState: PadButtonState = { down: [] };

export interface PadEdge {
  readonly state: PadButtonState;
  /** The button id that just went down, or null (no edge / not a button event). */
  readonly pressed: number | null;
}

export function padButtonEdge(state: PadButtonState, payload: UiGamepadPayload): PadEdge {
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
  readonly railIds: readonly string[];
  readonly selectedId: string;
  readonly modalOpen: boolean;
}

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
  return { state: edge.state, select: next === ctx.selectedId ? null : next };
}
