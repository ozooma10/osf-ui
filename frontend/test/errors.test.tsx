// @vitest-environment jsdom
//
// Settings surface failure paths: rejected writes, action timeout, capture-busy,
// and Escape peeling the undo overlay before it closes the surface.
//
// Every one of these is now a REJECTED request. Protocol 2.0 deleted the
// `ui.result` document with its `ok:false` field, so a refusal can no longer be
// mistaken for a success by a caller that forgot to inspect the reply — the
// promise rejects with a machine `code` and the view's `.catch` is the only
// path that can render it.

import { describe, it, expect, afterEach } from 'vitest';
import { makeBridge, mount, unmount, flush } from './helpers/settingsHarness';
import { WIDGETS, VIEWS } from './helpers/settingsFixtures';

afterEach(unmount);

async function mountKit() {
  // The registry and catalog arrive as replayed state, before the first paint.
  const bridge = makeBridge({ state: { 'osfui/settings': WIDGETS, 'osfui/views': VIEWS } });
  const el = await mount(bridge);
  await flush();
  [...el.querySelectorAll<HTMLButtonElement>('.rail-item')]
    .find((b) => b.textContent!.includes('Acme Kit'))!
    .click();
  await flush();
  return { bridge, el };
}

describe('settings.set rejection', () => {
  it('toasts writeRejected and abandons the save state', async () => {
    const { bridge, el } = await mountKit();
    el.querySelector<HTMLButtonElement>('#ctl-acme\\.kit-boolOn')!.click();
    await flush();

    // "Saving…" is showing while the write is in flight.
    const saveEl = el.querySelector('#save-state')!;
    expect(saveEl.classList.contains('visible')).toBe(true);

    const setIdx = bridge.indexOf('settings.set');
    // 2.0: a refusal REJECTS. There is no resolved `{ ok:false }` document.
    bridge.reject(setIdx, { code: 'invalid-value' });
    await flush();

    const toast = el.querySelector('.toast--danger')!;
    expect(toast).not.toBeNull();
    expect(toast.textContent).toContain('acme.kit.boolOn');
    expect(toast.textContent).toContain('invalid-value');

    expect(el.querySelector('#save-state')!.classList.contains('visible')).toBe(false);

    // What used to follow — a `settings.get` to pull authoritative state back —
    // is gone from the contract: the host republishes `osfui/settings` itself,
    // and every open document gets it. Modelled here as that republish.
    bridge.publish('osfui/settings', WIDGETS);
    await flush();
    expect(
      el.querySelector<HTMLButtonElement>('#ctl-acme\\.kit-boolOn')!.getAttribute('aria-checked'),
    ).toBe('true');
  });
});

describe('action timeout', () => {
  it('warns "No response from {mod}" and restores the button', async () => {
    const { bridge, el } = await mountKit();
    const go = [...el.querySelectorAll<HTMLButtonElement>('.row--action .osf-btn')].find(
      (b) => b.textContent === 'Run it',
    )!;
    go.click();
    await flush();
    expect(go.disabled).toBe(true);
    expect(go.classList.contains('pending')).toBe(true);

    // A schema `action` addresses the mod's own REQUEST endpoint.
    const idx = bridge.indexOf('acme.kit.run');
    bridge.reject(idx, { code: 'timeout' });
    await flush();

    const toast = el.querySelector('.toast--warn')!;
    expect(toast).not.toBeNull();
    expect(toast.textContent).toContain('No response from acme.kit');
    const restored = [...el.querySelectorAll<HTMLButtonElement>('.row--action .osf-btn')].find(
      (b) => b.textContent === 'Run it',
    )!;
    expect(restored.disabled).toBe(false);
    expect(restored.classList.contains('pending')).toBe(false);
  });
});

describe('capture-busy', () => {
  it('warns and restores the armed key button', async () => {
    const { bridge, el } = await mountKit();
    const keyBtn = el.querySelector<HTMLButtonElement>('#ctl-acme\\.kit-bindKey')!;
    keyBtn.click();
    await flush();
    // `.listening` is the class padnav suspends navigation on.
    expect(el.querySelector('.listening')).not.toBeNull();

    // `settings.captureKey` settles in MACHINE time: it either arms, or refuses
    // like this. The captured key would arrive separately, as an event.
    const idx = bridge.indexOf('settings.captureKey');
    bridge.reject(idx, { code: 'capture-busy' });
    await flush();

    const toast = el.querySelector('.toast--warn')!;
    expect(toast!.textContent).toContain('Another rebind is already listening.');
    expect(el.querySelector('.listening')).toBeNull();
    expect(el.querySelector<HTMLButtonElement>('#ctl-acme\\.kit-bindKey')!.textContent).toBe('K');
  });
});

describe('Escape peels the undo overlay before closing', () => {
  it('first Escape closes the panel; a second sends close', async () => {
    const { bridge, el } = await mountKit();
    el.querySelector<HTMLButtonElement>('#ctl-acme\\.kit-boolOn')!.click();
    await flush();
    el.querySelector<HTMLButtonElement>('#session-chip')!.click();
    await flush();
    expect(el.querySelector('.session-overlay')).not.toBeNull();

    const closesBefore = bridge.sent.filter((s) => s.name === 'close').length;

    // First Escape peels the overlay and must not close.
    document.dispatchEvent(new KeyboardEvent('keydown', { key: 'Escape', keyCode: 27 }));
    await flush();
    expect(el.querySelector('.session-overlay')).toBeNull();
    expect(bridge.sent.filter((s) => s.name === 'close').length).toBe(closesBefore);

    // Second Escape closes. `close` is a SEND: there is no outcome to await.
    document.dispatchEvent(new KeyboardEvent('keydown', { key: 'Escape', keyCode: 27 }));
    await flush();
    expect(bridge.sent.filter((s) => s.name === 'close').length).toBe(closesBefore + 1);
  });
});
