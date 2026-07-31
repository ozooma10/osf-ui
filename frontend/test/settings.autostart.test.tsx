// @vitest-environment jsdom
//
// Per-HUD "Start automatically" (protocol 1.6): rendered only for views the
// host marks autoStartMutable, saved via osfui.setViewAutoStart, applied at
// the next launch — the row never touches the immediate hud.show/hide path.

import { describe, it, expect, afterEach } from 'vitest';
import { makeBridge, mount, unmount, flush } from './helpers/settingsHarness';
import { WIDGETS, VIEWS } from './helpers/settingsFixtures';
import type { ViewsDataPayload } from '@sdk';

afterEach(unmount);

async function mountKit() {
  const bridge = makeBridge();
  const el = await mount(bridge);
  bridge.deliver('settings.data', WIDGETS);
  bridge.deliver('views.data', VIEWS);
  await flush();
  [...el.querySelectorAll<HTMLButtonElement>('.rail-item')]
    .find((b) => b.textContent!.includes('Acme Kit'))!
    .click();
  await flush();
  return { bridge, el };
}

function viewsWith(patch: Partial<ViewsDataPayload['views'][number]>): ViewsDataPayload {
  const copy = JSON.parse(JSON.stringify(VIEWS)) as ViewsDataPayload;
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

    // The host withdrawing eligibility (e.g. Debug mode turned off for a
    // debugOnly HUD) removes the row on the next catalog push.
    bridge.deliver('views.data', viewsWith({ autoStartMutable: false }));
    await flush();
    expect(el.querySelector('.autostart-row')).toBeNull();
  });

  it('saves through osfui.setViewAutoStart with an optimistic, disabled switch', async () => {
    const { bridge, el } = await mountKit();
    const sentBefore = bridge.sent.length;
    autoStartSwitch(el)!.click();
    await flush();

    const idx = bridge.indexOf('osfui.setViewAutoStart');
    expect(idx).toBeGreaterThanOrEqual(0);
    expect(bridge.requests[idx]!.fields).toEqual({ view: 'acme.kit/hud', enabled: true });
    // Next-launch policy only: no immediate hud.show/hide was fired.
    expect(bridge.sent.slice(sentBefore).some((s) => s.command.startsWith('hud.'))).toBe(false);

    // While the save is in flight the switch shows the requested position but
    // cannot be flipped again.
    const toggle = autoStartSwitch(el)!;
    expect(toggle.getAttribute('aria-checked')).toBe('true');
    expect(toggle.disabled).toBe(true);

    // The ack's views.data rebroadcast is authoritative and re-enables it.
    bridge.settle(idx, {});
    bridge.deliver('views.data', viewsWith({ autoStart: true }));
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
