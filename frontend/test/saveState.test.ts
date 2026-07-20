import { describe, it, expect } from 'vitest';
import {
  SAVE_FADE_MS,
  initialSaveState,
  isSaveStateVisible,
  saveStateAbandon,
  saveStateFaded,
  saveStatePending,
  saveStatePersisted,
  type SaveState,
} from '@lib/saveState';

const pendingIds = (s: SaveState): string[] => [...s.pending].sort();

describe('saveStatePending', () => {
  it('marks the mod and shows "Saving…"', () => {
    const out = saveStatePending(initialSaveState, 'acme.atlas');
    expect(pendingIds(out.state)).toEqual(['acme.atlas']);
    expect(out.state.label).toBe('saving');
    expect(out.state.classes).toEqual(['visible']);
    expect(isSaveStateVisible(out.state)).toBe(true);
  });

  it('cancels an armed fade and drops "done"', () => {
    let s = saveStatePending(initialSaveState, 'a').state;
    s = saveStatePersisted(s, 'a').state; // now "Saved", classes visible+done
    expect(s.classes).toEqual(['visible', 'done']);

    const out = saveStatePending(s, 'a');
    expect(out.cancelFade).toBe(true);
    expect(out.state.label).toBe('saving');
    expect(out.state.classes).toEqual(['visible']);
  });

  it('is idempotent for a mod already pending', () => {
    const once = saveStatePending(initialSaveState, 'a').state;
    const twice = saveStatePending(once, 'a').state;
    expect(pendingIds(twice)).toEqual(['a']);
  });
});

describe('saveStatePersisted', () => {
  it('fires ONLY when the set drains to empty', () => {
    let s = saveStatePending(initialSaveState, 'a').state;
    s = saveStatePending(s, 'b').state;

    // 'a' lands but 'b' is still outstanding: bookkeeping only.
    const first = saveStatePersisted(s, 'a');
    expect(pendingIds(first.state)).toEqual(['b']);
    expect(first.state.label).toBe('saving');
    expect(first.state.classes).toEqual(['visible']);
    expect(first.scheduleFadeMs).toBeNull();

    // 'b' drains the set: now "Saved".
    const second = saveStatePersisted(first.state, 'b');
    expect(pendingIds(second.state)).toEqual([]);
    expect(second.state.label).toBe('saved');
    expect(second.state.classes).toEqual(['visible', 'done']);
    expect(second.scheduleFadeMs).toBe(SAVE_FADE_MS);
    expect(SAVE_FADE_MS).toBe(1800);
  });

  it('does NOT clear the indicator — it swaps the text and adds classes', () => {
    // "persisted" is a success announcement, not a teardown: only the 1800ms
    // fade (or an abandon) hides it.
    const s = saveStatePending(initialSaveState, 'a').state;
    const out = saveStatePersisted(s, 'a');
    expect(isSaveStateVisible(out.state)).toBe(true);
    expect(out.state.classes).toContain('visible');
    expect(out.state.classes).toContain('done');
  });

  it('ignores a persisted push this view never asked for', () => {
    // A sibling DLL or another view wrote; don't claim its confirmation.
    const s = saveStatePending(initialSaveState, 'a').state;
    const out = saveStatePersisted(s, 'someone.else');
    expect(pendingIds(out.state)).toEqual(['a']);
    expect(out.state.label).toBe('saving');
    expect(out.scheduleFadeMs).toBeNull();
  });

  it('ignores a duplicate persisted push for a mod already drained', () => {
    let s = saveStatePending(initialSaveState, 'a').state;
    s = saveStatePersisted(s, 'a').state;
    const again = saveStatePersisted(s, 'a');
    expect(again.scheduleFadeMs).toBeNull();
    expect(again.state.label).toBe('saved'); // unchanged
  });

  it('hides the indicator once the fade timer fires', () => {
    let s = saveStatePending(initialSaveState, 'a').state;
    s = saveStatePersisted(s, 'a').state;
    s = saveStateFaded(s);
    expect(s.classes).toEqual([]);
    expect(isSaveStateVisible(s)).toBe(false);
    // The label is not reset — the fade only clears classes.
    expect(s.label).toBe('saved');
  });
});

describe('saveStateAbandon', () => {
  it('is the transition that CLEARS the indicator', () => {
    const s = saveStatePending(initialSaveState, 'a').state;
    const out = saveStateAbandon(s, 'a');
    expect(pendingIds(out.state)).toEqual([]);
    expect(out.state.classes).toEqual([]);
    expect(isSaveStateVisible(out.state)).toBe(false);
  });

  it('leaves the stale "Saving…" label in place under the hidden element', () => {
    // Quirk: abandon clears the classes only, never the label text.
    const s = saveStatePending(initialSaveState, 'a').state;
    expect(saveStateAbandon(s, 'a').state.label).toBe('saving');
  });

  it('does not cancel an armed fade timer', () => {
    // Quirk: unlike pending/persisted, abandon never clears the fade timer.
    // Harmless — the timer removes exactly the classes abandon just removed.
    const s = saveStatePending(initialSaveState, 'a').state;
    expect(saveStateAbandon(s, 'a').cancelFade).toBe(false);
  });

  it('keeps "Saving…" up while OTHER writes to the same mod are pending', () => {
    let s = saveStatePending(initialSaveState, 'a').state;
    s = saveStatePending(s, 'b').state;
    const out = saveStateAbandon(s, 'a');
    expect(pendingIds(out.state)).toEqual(['b']);
    expect(isSaveStateVisible(out.state)).toBe(true);
  });

  it('drops the entry so a later persisted push finds nothing', () => {
    // Losing the confirmation is the intended trade: never show a false one.
    let s = saveStatePending(initialSaveState, 'a').state;
    s = saveStatePending(s, 'a').state; // two in-flight writes, one set entry
    s = saveStateAbandon(s, 'a').state; // one rejected -> entry gone
    expect(pendingIds(s)).toEqual([]);

    const late = saveStatePersisted(s, 'a');
    expect(late.state.label).toBe('saving'); // never flips to "Saved"
    expect(late.scheduleFadeMs).toBeNull();
    expect(late.state.classes).toEqual([]);
  });

  it('ignores an abandon for a mod that was never pending', () => {
    const s = saveStatePending(initialSaveState, 'a').state;
    const out = saveStateAbandon(s, 'ghost');
    expect(out.state.classes).toEqual(['visible']);
    expect(pendingIds(out.state)).toEqual(['a']);
  });
});

describe('immutability', () => {
  it('never mutates the input state or its set', () => {
    const s = saveStatePending(initialSaveState, 'a').state;
    const snapshot = pendingIds(s);
    saveStatePersisted(s, 'a');
    saveStateAbandon(s, 'a');
    saveStatePending(s, 'b');
    expect(pendingIds(s)).toEqual(snapshot);
    expect(initialSaveState.pending.size).toBe(0);
  });
});
