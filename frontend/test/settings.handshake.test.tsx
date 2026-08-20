// @vitest-environment jsdom

import { describe, it, expect, afterEach } from 'vitest';
import { makeBridge, mount, unmount, flush } from './helpers/settingsHarness';
import { WIDGETS, VIEWS } from './helpers/settingsFixtures';

afterEach(unmount);

/** What the OSF UI runtime has already replayed before this document's first paint. */
const REPLAY = { 'osfui/settings': WIDGETS, 'osfui/views': VIEWS };

/** The reads 2.0 deleted. None of these may ever leave the view again. */
const DELETED_READS = ['settings.get', 'views.get', 'diagnostics.get', 'i18n.get'];

describe('state-only startup', () => {
  it('issues no read at all — subscribing is the read', async () => {
    const bridge = makeBridge({ state: REPLAY });
    await mount(bridge);
    await flush();

    const names = bridge.outbound.map((m) => m.name);
    for (const read of DELETED_READS) expect(names).not.toContain(read);
    expect(names).toEqual(['osfui.handleBack']);
  });

  it('paints the replayed registry and catalog on the FIRST render', async () => {
    const bridge = makeBridge({ state: REPLAY });
    const el = await mount(bridge);

    expect(el.querySelectorAll('.rail-item').length).toBeGreaterThan(0);
    expect(el.textContent).toContain('Acme Kit');
  });

  it('leaves the release-version badge blank until diagnostics state arrives', async () => {
    const bridge = makeBridge({ state: REPLAY });
    const el = await mount(bridge);
    await flush();

    expect(el.textContent).not.toMatch(/\bv1\.0\.0\b/);
    bridge.publish('osfui/diagnostics', { system: { version: '1.0.0' }, issues: [] });
    await flush();
    expect(el.textContent).toContain('v1.0.0');
  });
});

describe('replayed diagnostics', () => {
  it('feeds the OSF UI release-version badge without issuing a read', async () => {
    const bridge = makeBridge({ state: REPLAY, version: '2.0.0' });
    const el = await mount(bridge);
    await flush();

    expect(el.textContent).toContain('v2.0.0');
    // Still no read: retained state is already the complete value.
    expect(bridge.outbound.map((m) => m.name)).toEqual(['osfui.handleBack']);
  });
});

describe('a bridge that reports itself unavailable', () => {
  it('still paints the replayed state, and stays silent on the wire', async () => {
    const bridge = makeBridge({ state: REPLAY, available: false });
    const el = await mount(bridge);
    await flush();

    expect(el.querySelectorAll('.rail-item').length).toBeGreaterThan(0);
    expect(el.textContent).toContain('Acme Kit');
    expect(bridge.outbound).toEqual([]);
  });
});
