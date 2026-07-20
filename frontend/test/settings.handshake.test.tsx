// @vitest-environment jsdom
//
// settings.handshake.test.tsx — the view must not depend on `runtime.ready`
// for anything but the version badge.
//
// REGRESSION (2026-07-19, in-game "F10 opens an empty Mods surface"):
// `runtime.ready` is a one-shot greeting the runtime emits during its own
// initialization. On the out-of-process WebView2 backend the host that carries
// it does not exist yet at that moment, so the greeting can be missed
// entirely. The view used to gate its initial `settings.get` / `views.get` on
// that promise, so a missed greeting meant the reads were never sent, no data
// ever arrived, and the rail stayed empty forever — with no error anywhere.
//
// The transport gap is fixed on the native side (pre-connect bridge messages
// are queued and flushed), but the view must not be one dropped message away
// from useless, so the contract is pinned here.

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

    // The runtime answers the gets; nothing about that path involves `ready`.
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

    // Whatever chrome carries the host version must not claim a version it
    // never learned (the badge suppresses itself before the handshake).
    expect(el.textContent).not.toMatch(/\b1\.0\.0\b/);
  });
});
