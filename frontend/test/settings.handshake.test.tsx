// @vitest-environment jsdom
//
// The view must not depend on `runtime.ready` for anything but the version
// badge.
//
// Regression (2026-07-19, in-game "F10 opens an empty Mods surface"):
// `runtime.ready` is a one-shot greeting emitted during runtime init. On the
// out-of-process WebView2 backend the host that carries it may not exist yet,
// so the greeting can be missed. The view used to gate its initial
// `settings.get` / `views.get` on that promise, so a missed greeting meant the
// reads were never sent and the rail stayed empty, with no error anywhere.
// The transport gap is fixed natively (pre-connect bridge messages are queued
// and flushed); this pins the view-side contract too.

import { describe, it, expect, afterEach } from 'vitest';
import { makeBridge, mount, unmount, flush } from './helpers/settingsHarness';
import { WIDGETS, VIEWS } from './helpers/settingsFixtures';

afterEach(unmount);

describe('a runtime.ready that never arrives', () => {
  it('still issues the initial settings.get and views.get', async () => {
    const bridge = makeBridge({ readyNeverResolves: true });
    await mount(bridge);
    await flush();

    const commands = bridge.sent.map((s) => s.command);
    expect(commands).toContain('settings.get');
    expect(commands).toContain('views.get');
  });

  it('renders pushed data — the rail and the launch cards populate', async () => {
    const bridge = makeBridge({ readyNeverResolves: true });
    const el = await mount(bridge);

    // The runtime answers the gets; that path never involves `ready`.
    bridge.emit('settings.data', WIDGETS);
    bridge.emit('views.data', VIEWS);
    await flush();

    expect(el.querySelectorAll('.rail-item').length).toBeGreaterThan(0);
    expect(el.textContent).toContain('Acme Kit');
  });

  it('leaves only the version badge blank', async () => {
    const bridge = makeBridge({ readyNeverResolves: true });
    const el = await mount(bridge);
    bridge.emit('settings.data', WIDGETS);
    bridge.emit('views.data', VIEWS);
    await flush();

    // The badge suppresses itself before the handshake rather than claiming a
    // version it never learned.
    expect(el.textContent).not.toMatch(/\b1\.0\.0\b/);
  });
});
