import { describe, it, expect } from 'vitest';
import {
  HOME_ID,
  PAD_LSHOULDER,
  PAD_RSHOULDER,
  cycleRail,
  initialPadButtonState,
  padButtonEdge,
  reduceGamepad,
  reduceVisibility,
  type LifecycleState,
  type PadButtonState,
  type RailCycleContext,
} from '@lib/lifecycle';
import type { UiGamepadPayload } from '@sdk';

const button = (id: number, down: boolean): UiGamepadPayload => ({
  kind: 'button',
  button: { id, down },
});

describe('reduceVisibility', () => {
  it('does nothing at all on a hide edge', () => {
    const state: LifecycleState = { selectedId: 'acme.atlas', filter: 'star' };
    const out = reduceVisibility(state, { visible: false });
    expect(out.state).toBe(state); // same reference: no re-render
    expect(out).toMatchObject({ clearBaseline: false, reselect: false, resetPadnav: false });
  });

  it('resets selection and filter on a show edge', () => {
    const out = reduceVisibility({ selectedId: 'acme.atlas', filter: 'star' }, { visible: true });
    expect(out.state).toEqual({ selectedId: HOME_ID, filter: '' });
    expect(out.reselect).toBe(true);
    expect(out.clearBaseline).toBe(true);
    expect(out.resetPadnav).toBe(true);
  });

  it('reselects when already on Home but a filter is active', () => {
    const out = reduceVisibility({ selectedId: HOME_ID, filter: 'atlas' }, { visible: true });
    expect(out.state).toEqual({ selectedId: HOME_ID, filter: '' });
    expect(out.reselect).toBe(true);
  });

  it('skips the reselect (and the pane rebuild) when already on a clean Home', () => {
    const state: LifecycleState = { selectedId: HOME_ID, filter: '' };
    const out = reduceVisibility(state, { visible: true });
    expect(out.state).toBe(state);
    expect(out.reselect).toBe(false);
    // ...but the baseline and padnav reset still happen: the undo scope is the
    // VISIT, and the gamepad resume point starts over regardless.
    expect(out.clearBaseline).toBe(true);
    expect(out.resetPadnav).toBe(true);
  });

  it('always clears the baseline on a show edge, whatever the selection', () => {
    for (const state of [
      { selectedId: HOME_ID, filter: '' },
      { selectedId: HOME_ID, filter: 'x' },
      { selectedId: 'osfui', filter: '' },
    ]) {
      expect(reduceVisibility(state, { visible: true }).clearBaseline).toBe(true);
    }
  });
});

describe('padButtonEdge', () => {
  it('reports the down edge once and ignores repeats', () => {
    let state: PadButtonState = initialPadButtonState;

    let out = padButtonEdge(state, button(PAD_RSHOULDER, true));
    expect(out.pressed).toBe(PAD_RSHOULDER);
    state = out.state;

    // Held: the runtime may re-report the same button on later polls.
    out = padButtonEdge(state, button(PAD_RSHOULDER, true));
    expect(out.pressed).toBeNull();
    state = out.state;

    out = padButtonEdge(state, button(PAD_RSHOULDER, false));
    expect(out.pressed).toBeNull();
    state = out.state;

    // Released, so the next press is a fresh edge again.
    expect(padButtonEdge(state, button(PAD_RSHOULDER, true)).pressed).toBe(PAD_RSHOULDER);
  });

  it('tracks buttons independently', () => {
    const a = padButtonEdge(initialPadButtonState, button(PAD_LSHOULDER, true));
    expect(a.pressed).toBe(PAD_LSHOULDER);
    const b = padButtonEdge(a.state, button(PAD_RSHOULDER, true));
    expect(b.pressed).toBe(PAD_RSHOULDER);
    expect([...b.state.down].sort()).toEqual([PAD_LSHOULDER, PAD_RSHOULDER].sort());
  });

  it('survives a malformed native push instead of throwing', () => {
    // The legacy guard tested `!p` and `!p.button` explicitly
    // (main.legacy.js:1867). These payloads are ill-typed on purpose: they
    // model a bridge frame that does not match its declared shape, which must
    // be ignored rather than throw and kill the subscription.
    for (const bad of [null, undefined, {}, { kind: 'button' }]) {
      const out = padButtonEdge(initialPadButtonState, bad as unknown as UiGamepadPayload);
      expect(out.pressed).toBeNull();
      expect(out.state).toBe(initialPadButtonState);
    }
  });

  it('ignores an unmatched release and stick events', () => {
    expect(padButtonEdge(initialPadButtonState, button(PAD_LSHOULDER, false)).pressed).toBeNull();
    const stick: UiGamepadPayload = { kind: 'stick', axes: { lx: 1, ly: 0, rx: 0, ry: 0 } };
    const out = padButtonEdge(initialPadButtonState, stick);
    expect(out.pressed).toBeNull();
    expect(out.state).toBe(initialPadButtonState);
  });
});

describe('cycleRail', () => {
  const ids = [HOME_ID, 'osfui', 'acme.atlas', 'acme.shipworks'];

  it('steps forward and backward, wrapping at both ends', () => {
    expect(cycleRail(ids, HOME_ID, 1)).toBe('osfui');
    expect(cycleRail(ids, 'acme.shipworks', 1)).toBe(HOME_ID);
    expect(cycleRail(ids, HOME_ID, -1)).toBe('acme.shipworks');
    expect(cycleRail(ids, 'osfui', -1)).toBe(HOME_ID);
  });

  it('falls to the FIRST entry regardless of direction when the selection is gone', () => {
    // Quirk (main.legacy.js:1886-1887): indexOf -> -1 is treated as index 0
    // *before* delta is applied, so LB and RB both land on ids[0].
    expect(cycleRail(ids, 'filtered-away', 1)).toBe(HOME_ID);
    expect(cycleRail(ids, 'filtered-away', -1)).toBe(HOME_ID);
  });

  it('returns the current id for an empty rail', () => {
    expect(cycleRail([], 'osfui', 1)).toBe('osfui');
  });
});

describe('reduceGamepad', () => {
  const ctx = (over: Partial<RailCycleContext> = {}): RailCycleContext => ({
    railIds: [HOME_ID, 'osfui', 'acme.atlas'],
    selectedId: HOME_ID,
    modalOpen: false,
    ...over,
  });

  it('cycles the rail on an LB/RB down edge', () => {
    expect(reduceGamepad(initialPadButtonState, button(PAD_RSHOULDER, true), ctx()).select).toBe(
      'osfui',
    );
    expect(reduceGamepad(initialPadButtonState, button(PAD_LSHOULDER, true), ctx()).select).toBe(
      'acme.atlas',
    );
  });

  it('ignores button-up and repeat frames', () => {
    const down = reduceGamepad(initialPadButtonState, button(PAD_RSHOULDER, true), ctx());
    expect(down.select).toBe('osfui');
    // Repeat while held: no second step.
    expect(reduceGamepad(down.state, button(PAD_RSHOULDER, true), ctx()).select).toBeNull();
    // Release: never a step.
    expect(reduceGamepad(down.state, button(PAD_RSHOULDER, false), ctx()).select).toBeNull();
  });

  it('ignores every other button', () => {
    for (const id of [0x1000, 0x2000, 0x0001]) {
      expect(reduceGamepad(initialPadButtonState, button(id, true), ctx()).select).toBeNull();
    }
  });

  it('is SUPPRESSED while a modal is open', () => {
    const out = reduceGamepad(
      initialPadButtonState,
      button(PAD_RSHOULDER, true),
      ctx({ modalOpen: true }),
    );
    expect(out.select).toBeNull();
    // The edge is still consumed, so re-pressing after the modal closes works
    // but holding through the close does not fire.
    expect(out.state.down).toContain(PAD_RSHOULDER);
    expect(reduceGamepad(out.state, button(PAD_RSHOULDER, true), ctx()).select).toBeNull();
  });

  it('does not re-select when the rail cannot move', () => {
    const out = reduceGamepad(
      initialPadButtonState,
      button(PAD_RSHOULDER, true),
      ctx({ railIds: [HOME_ID], selectedId: HOME_ID }),
    );
    expect(out.select).toBeNull();
  });
});
