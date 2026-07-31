// @vitest-environment jsdom
//
// The Mods surface issues NO read, and depends on the handshake for nothing but
// the version badge.
//
// Regression history (2026-07-19, in-game "F10 opens an empty Mods surface"):
// the greeting is a one-shot, and on the out-of-process WebView2 backend the
// host that carries it may not exist yet, so it could be missed. The 1.x view
// gated its initial `settings.get` / `views.get` on that promise, so a missed
// greeting meant the reads were never sent, the rail stayed empty, and nothing
// anywhere reported an error. The 1.x fix was to re-issue the reads on the
// `runtime.ready` edge, and this file pinned that re-issue.
//
// Protocol 2.0 deletes the failure mode instead of patching it: the four
// catalogs are STATE keys replayed to every fresh document, and subscribing IS
// the read. So this file now pins the opposite of what it used to — that no
// read is issued on any path, and that the surface paints from the replay
// alone. `settings.get` / `views.get` / `diagnostics.get` / `i18n.get` are not
// endpoints any more; a view that still called one would be answered
// "unknown-endpoint".

import { describe, it, expect, afterEach } from 'vitest';
import { makeBridge, mount, unmount, flush } from './helpers/settingsHarness';
import { WIDGETS, VIEWS } from './helpers/settingsFixtures';

afterEach(unmount);

/** What the host has already replayed to this document before the first paint. */
const REPLAY = { 'osfui/settings': WIDGETS, 'osfui/views': VIEWS };

/** The reads 2.0 deleted. None of these may ever leave the view again. */
const DELETED_READS = ['settings.get', 'views.get', 'diagnostics.get', 'i18n.get'];

describe('a handshake that never completes', () => {
  it('issues no read at all — subscribing is the read', async () => {
    const bridge = makeBridge({ state: REPLAY, readyNeverResolves: true });
    await mount(bridge);
    await flush();

    const names = bridge.outbound.map((m) => m.name);
    for (const read of DELETED_READS) expect(names).not.toContain(read);
    // Positively: the only thing the surface says on mount is the back-action
    // claim. Everything it KNOWS arrived without asking.
    expect(names).toEqual(['osfui.handleBack']);
  });

  it('paints the replayed registry and catalog on the FIRST render', async () => {
    // No deliver/publish here on purpose: the state was already present when the
    // subscription was made, so the very first paint is populated. This is what
    // replaces "gate the reads on ready, then render the replies".
    const bridge = makeBridge({ state: REPLAY, readyNeverResolves: true });
    const el = await mount(bridge);

    expect(el.querySelectorAll('.rail-item').length).toBeGreaterThan(0);
    expect(el.textContent).toContain('Acme Kit');
  });

  it('leaves only the version badge blank', async () => {
    const bridge = makeBridge({ state: REPLAY, readyNeverResolves: true });
    const el = await mount(bridge);
    await flush();

    // The badge suppresses itself before the handshake rather than claiming a
    // version it never learned.
    expect(el.textContent).not.toMatch(/\bv1\.0\.0\b/);
  });

  it('survives a ready() that REJECTS, which is what standalone does', async () => {
    // The 2.0 helper rejects `ready` with code "no-bridge" in a plain browser
    // rather than hanging. An unhandled rejection here would take the view down
    // on first paint.
    const bridge = makeBridge({ state: REPLAY, readyRejects: true });
    const el = await mount(bridge);
    await flush();

    expect(el.textContent).toContain('Acme Kit');
    expect(el.textContent).not.toMatch(/\bv1\.0\.0\b/);
  });
});

describe('a completed handshake', () => {
  it('feeds the version badge, and nothing else', async () => {
    const bridge = makeBridge({ state: REPLAY, version: '2.0.0' });
    const el = await mount(bridge);
    await flush();

    expect(el.textContent).toContain('v2.0.0');
    // Still no read: a resolved handshake is not a cue to fetch anything.
    expect(bridge.outbound.map((m) => m.name)).toEqual(['osfui.handleBack']);
  });
});

describe('a bridge that reports itself unavailable', () => {
  it('still paints the replayed state, and stays silent on the wire', async () => {
    // The 1.x version of this case was a race: the availability check ran before
    // the transport came up, so the reads were skipped and had to be re-issued
    // on the `runtime.ready` edge. There is nothing to re-issue now, so the only
    // remaining invariant is that rendering never depends on `available()`.
    const bridge = makeBridge({ state: REPLAY, available: false });
    const el = await mount(bridge);
    await flush();

    expect(el.querySelectorAll('.rail-item').length).toBeGreaterThan(0);
    expect(el.textContent).toContain('Acme Kit');
    expect(bridge.outbound).toEqual([]);
  });
});
