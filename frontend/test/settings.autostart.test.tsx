// @vitest-environment jsdom

import { describe, it, expect, afterEach } from 'vitest';
import { makeBridge, mount, unmount, flush } from './helpers/settingsHarness';
import { WIDGETS, VIEWS } from './helpers/settingsFixtures';
import type { ViewsData } from '@sdk';

afterEach(unmount);

/** Endpoints that would open or close the HUD now. None may come from this row. */
const VISIBILITY_ENDPOINTS = ['menu.open', 'menu.close', 'setViewHidden', 'hud.show', 'hud.hide'];

async function mountKit() {
  const bridge = makeBridge({ state: { 'osfui/settings': WIDGETS, 'osfui/views': VIEWS } });
  const el = await mount(bridge);
  await flush();
  [...el.querySelectorAll<HTMLButtonElement>('.rail-item')]
    .find((b) => b.textContent!.includes('Acme Kit'))!
    .click();
  await flush();
  return { bridge, el };
}

function viewsWith(patch: Partial<ViewsData['views'][number]>): ViewsData {
  const copy = JSON.parse(JSON.stringify(VIEWS)) as ViewsData;
  const hud = copy.views.find((v) => v.id === 'acme.kit/hud')!;
  Object.assign(hud, patch);
  return copy;
}

const autoStartSwitch = (el: HTMLElement) =>
  el.querySelector<HTMLButtonElement>('.autostart-row .osf-switch');

describe('start automatically row', () => {
  it('renders only for autoStartMutable views and reflects the effective policy', async () => {
    const { bridge, el } = await mountKit();
    const row = el.querySelector<HTMLElement>('.autostart-row')!;
    expect(row).not.toBeNull();
    expect(row.textContent).toContain('Start automatically');
    expect(autoStartSwitch(el)!.getAttribute('aria-checked')).toBe('false');
    // Exactly one: the menu row and the ineligible views get none.
    expect(el.querySelectorAll('.autostart-row').length).toBe(1);

    bridge.publish('osfui/views', viewsWith({ autoStartMutable: false }));
    await flush();
    expect(el.querySelector('.autostart-row')).toBeNull();
  });

  it('saves through osfui.setViewAutoStart with an optimistic, disabled switch', async () => {
    const { bridge, el } = await mountKit();
    const outboundBefore = bridge.outbound.length;
    autoStartSwitch(el)!.click();
    await flush();

    const idx = bridge.indexOf('osfui.setViewAutoStart');
    expect(idx).toBeGreaterThanOrEqual(0);
    expect(bridge.requests[idx]!.payload).toEqual({ view: 'acme.kit/hud', enabled: true });
    // Next-launch policy only: nothing that shows or hides the HUD went out.
    const after = bridge.outbound.slice(outboundBefore).map((m) => m.name);
    for (const endpoint of VISIBILITY_ENDPOINTS) expect(after).not.toContain(endpoint);

    const toggle = autoStartSwitch(el)!;
    expect(toggle.getAttribute('aria-checked')).toBe('true');
    expect(toggle.disabled).toBe(true);

    bridge.settle(idx, {});
    bridge.publish('osfui/views', viewsWith({ autoStart: true }));
    await flush();
    const settled = autoStartSwitch(el)!;
    expect(settled.getAttribute('aria-checked')).toBe('true');
    expect(settled.disabled).toBe(false);
  });

  it('reverts to the server value and toasts when persistence fails', async () => {
    const { bridge, el } = await mountKit();
    autoStartSwitch(el)!.click();
    await flush();

    bridge.reject(bridge.indexOf('osfui.setViewAutoStart'), { code: 'persistence-failed' });
    await flush();

    const toast = el.querySelector('.toast--danger')!;
    expect(toast).not.toBeNull();
    expect(toast.textContent).toContain('persistence-failed');

    // Back on the last authoritative value, ready for another attempt.
    const toggle = autoStartSwitch(el)!;
    expect(toggle.getAttribute('aria-checked')).toBe('false');
    expect(toggle.disabled).toBe(false);
  });
});
