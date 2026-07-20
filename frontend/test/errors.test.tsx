// @vitest-environment jsdom
//
// Settings surface failure paths: rejected writes, action timeout, capture-busy,
// and Escape peeling the undo overlay before it closes the surface.

import { describe, it, expect, afterEach } from 'vitest';
import { makeBridge, mount, unmount, flush } from './helpers/settingsHarness';
import { WIDGETS, VIEWS } from './helpers/settingsFixtures';

afterEach(unmount);

async function mountKit() {
  const bridge = makeBridge();
  const el = await mount(bridge);
  bridge.emit('settings.data', WIDGETS);
  bridge.emit('views.data', VIEWS);
  await flush();
  [...el.querySelectorAll<HTMLButtonElement>('.rail-item')]
    .find((b) => b.textContent!.includes('Acme Kit'))!
    .click();
  await flush();
  return { bridge, el };
}

describe('settings.set rejection', () => {
  it('toasts writeRejected, abandons the save state, and re-sends settings.get', async () => {
    const { bridge, el } = await mountKit();
    el.querySelector<HTMLButtonElement>('#ctl-acme\\.kit-boolOn')!.click();
    await flush();

    // "Saving…" is showing while the write is in flight.
    const saveEl = el.querySelector('#save-state')!;
    expect(saveEl.classList.contains('visible')).toBe(true);

    const setIdx = bridge.indexOf('settings.set');
    bridge.reject(setIdx, { code: 'invalid-value' });
    await flush();

    const toast = el.querySelector('.toast--danger')!;
    expect(toast).not.toBeNull();
    expect(toast.textContent).toContain('acme.kit.boolOn');
    expect(toast.textContent).toContain('invalid-value');

    expect(el.querySelector('#save-state')!.classList.contains('visible')).toBe(false);

    expect(bridge.sent.some((s) => s.command === 'settings.get')).toBe(true);
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

    const closesBefore = bridge.sent.filter((s) => s.command === 'close').length;

    // First Escape peels the overlay and must not close.
    document.dispatchEvent(new KeyboardEvent('keydown', { key: 'Escape', keyCode: 27 }));
    await flush();
    expect(el.querySelector('.session-overlay')).toBeNull();
    expect(bridge.sent.filter((s) => s.command === 'close').length).toBe(closesBefore);

    // Second Escape closes.
    document.dispatchEvent(new KeyboardEvent('keydown', { key: 'Escape', keyCode: 27 }));
    await flush();
    expect(bridge.sent.filter((s) => s.command === 'close').length).toBe(closesBefore + 1);
  });
});
