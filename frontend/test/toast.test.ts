import { describe, it, expect, vi, afterEach } from 'vitest';
import {
  TOAST_LEAVING_MS,
  TOAST_REMOVE_MS,
  addToast,
  expireToast,
  initialToastState,
  removeToast,
  toastClassName,
  type ToastState,
} from '@lib/toast';

afterEach(() => {
  vi.useRealTimers();
});

/**
 * Drives the state machine the way a component would: arms each returned timer
 * with `setTimeout`, then lets fake timers advance, so the 2600/3000
 * assertions are measured rather than restated.
 */
function driver(): { get: () => ToastState; add: (m: string) => number } {
  let state = initialToastState;
  return {
    get: () => state,
    add(message: string): number {
      const out = addToast(state, message);
      state = out.state;
      for (const timer of out.timers) {
        setTimeout(() => {
          state = timer.action === 'leaving' ? expireToast(state, timer.id) : removeToast(state, timer.id);
        }, timer.delayMs);
      }
      return out.entry.id;
    },
  };
}

describe('timings', () => {
  it('pins the exact millisecond constants', () => {
    // Paired with the .leaving CSS transition in osfui.css; the 400ms gap
    // between them is the fade window. Do not change one alone.
    expect(TOAST_LEAVING_MS).toBe(2600);
    expect(TOAST_REMOVE_MS).toBe(3000);
  });

  it('schedules leaving at 2600ms and removal at 3000ms from insertion', () => {
    const out = addToast(initialToastState, 'Saved');
    expect(out.timers).toEqual([
      { id: out.entry.id, delayMs: 2600, action: 'leaving' },
      { id: out.entry.id, delayMs: 3000, action: 'remove' },
    ]);
  });

  it('applies `leaving` at 2600ms, not before', () => {
    vi.useFakeTimers();
    const d = driver();
    d.add('Rejected');

    vi.advanceTimersByTime(2599);
    expect(d.get().entries[0]?.leaving).toBe(false);

    vi.advanceTimersByTime(1);
    expect(d.get().entries[0]?.leaving).toBe(true);
    expect(d.get().entries).toHaveLength(1); // still mounted: fade window
  });

  it('removes the entry at 3000ms, not before', () => {
    vi.useFakeTimers();
    const d = driver();
    d.add('Rejected');

    vi.advanceTimersByTime(2999);
    expect(d.get().entries).toHaveLength(1);

    vi.advanceTimersByTime(1);
    expect(d.get().entries).toHaveLength(0);
  });

  it('measures both timers from EACH toast\'s own insertion, independently', () => {
    vi.useFakeTimers();
    const d = driver();
    d.add('first');
    vi.advanceTimersByTime(1000);
    d.add('second');

    // t=2600: only the first is leaving; the second's own clock is at 1600.
    vi.advanceTimersByTime(1600);
    expect(d.get().entries.map((e) => e.leaving)).toEqual([true, false]);

    // t=3000: the first is removed, the second is still fully opaque.
    vi.advanceTimersByTime(400);
    expect(d.get().entries.map((e) => [e.message, e.leaving])).toEqual([['second', false]]);

    // t=3600 = the second's own 2600ms.
    vi.advanceTimersByTime(600);
    expect(d.get().entries.map((e) => [e.message, e.leaving])).toEqual([['second', true]]);

    // t=4000 = the second's own 3000ms.
    vi.advanceTimersByTime(400);
    expect(d.get().entries).toHaveLength(0);
  });
});

describe('list semantics', () => {
  it('appends newest last and never reuses ids', () => {
    const a = addToast(initialToastState, 'one');
    const b = addToast(a.state, 'two', 'warn');
    expect(b.state.entries.map((e) => e.message)).toEqual(['one', 'two']);
    expect(b.entry.id).not.toBe(a.entry.id);

    const afterRemove = removeToast(b.state, b.entry.id);
    const c = addToast(afterRemove, 'three');
    expect(c.entry.id).not.toBe(b.entry.id);
  });

  it('does not de-duplicate identical messages', () => {
    // A burst of rejected writes really does show one toast each.
    const a = addToast(initialToastState, 'Rejected');
    const b = addToast(a.state, 'Rejected');
    expect(b.state.entries).toHaveLength(2);
  });

  it('omits `kind` entirely when none was given', () => {
    const out = addToast(initialToastState, 'plain');
    expect('kind' in out.entry).toBe(false);
  });

  it('leaves state untouched for unknown ids', () => {
    const a = addToast(initialToastState, 'one');
    expect(expireToast(a.state, 999)).toBe(a.state);
    expect(removeToast(a.state, 999)).toBe(a.state);
    // A second expire of the same id is a no-op too.
    const expired = expireToast(a.state, a.entry.id);
    expect(expireToast(expired, a.entry.id)).toBe(expired);
  });

  it('removes only the targeted entry', () => {
    const a = addToast(initialToastState, 'one');
    const b = addToast(a.state, 'two');
    const c = addToast(b.state, 'three');
    expect(removeToast(c.state, b.entry.id).entries.map((e) => e.message)).toEqual([
      'one',
      'three',
    ]);
  });
});

describe('toastClassName', () => {
  it('matches the legacy class concatenation', () => {
    const plain = addToast(initialToastState, 'x');
    expect(toastClassName(plain.entry)).toBe('toast');

    const danger = addToast(initialToastState, 'x', 'danger');
    expect(toastClassName(danger.entry)).toBe('toast toast--danger');

    const leaving = expireToast(danger.state, danger.entry.id).entries[0];
    expect(leaving && toastClassName(leaving)).toBe('toast toast--danger leaving');
  });
});
