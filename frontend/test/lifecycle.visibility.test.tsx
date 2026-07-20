// @vitest-environment jsdom
//
// The overlay open-edge reset. A `ui.visibility` push with `visible: true`
// means a fresh visit began (see @lib/lifecycle):
//   * the undo baseline is dropped (undo scopes to one visit);
//   * the filter is cleared and the selection returns to Home, but only when it
//     was not already there;
//   * padnav.reset() fires even on that no-op path;
//   * a `visible: false` push changes nothing.

import { describe, it, expect, afterEach, vi } from 'vitest';
import { makeBridge, mount, unmount, flush, typeFilter } from './helpers/settingsHarness';
import { WIDGETS, VIEWS } from './helpers/settingsFixtures';

afterEach(() => {
  unmount();
  delete (window as { padnav?: unknown }).padnav;
});

function selectRail(el: HTMLElement, label: string) {
  [...el.querySelectorAll<HTMLButtonElement>('.rail-item')]
    .find((b) => b.textContent!.includes(label))!
    .click();
}

describe('ui.visibility open edge', () => {
  it('clears the filter and returns the selection to Home', async () => {
    const bridge = makeBridge();
    const el = await mount(bridge);
    bridge.emit('settings.data', WIDGETS);
    bridge.emit('views.data', VIEWS);
    await flush();

    // Move off Home and set a filter.
    selectRail(el, 'Acme Kit');
    await typeFilter(el, 'slider');
    expect(el.querySelector('.search-results')).not.toBeNull();

    bridge.emit('ui.visibility', { visible: true });
    await flush();

    expect(el.querySelector<HTMLInputElement>('#filter')!.value).toBe('');
    expect(el.querySelector('.rail-item--home.selected')).not.toBeNull();
    expect(el.querySelector('.detail-body--home')).not.toBeNull();
  });

  it('drops the undo baseline so the chip disappears', async () => {
    const bridge = makeBridge();
    const el = await mount(bridge);
    bridge.emit('settings.data', WIDGETS);
    bridge.emit('views.data', VIEWS);
    await flush();

    selectRail(el, 'Acme Kit');
    await flush();
    el.querySelector<HTMLButtonElement>('#ctl-acme\\.kit-boolOn')!.click();
    await flush();
    const chip = el.querySelector<HTMLButtonElement>('#session-chip')!;
    expect(chip.style.display).not.toBe('none'); // visible after a change

    bridge.emit('ui.visibility', { visible: true });
    await flush();
    // Baseline gone -> zero changes -> chip hidden.
    expect(el.querySelector<HTMLButtonElement>('#session-chip')!.style.display).toBe('none');
  });

  it('calls padnav.reset() even when already on Home (the unconditional path)', async () => {
    const reset = vi.fn();
    (window as { padnav?: unknown }).padnav = { reset };
    const bridge = makeBridge();
    const el = await mount(bridge);
    bridge.emit('settings.data', WIDGETS);
    await flush();
    // Already on Home with an empty filter: the reselect is a no-op.
    expect(el.querySelector('.rail-item--home.selected')).not.toBeNull();

    bridge.emit('ui.visibility', { visible: true });
    await flush();
    // padnav.reset still fires.
    expect(reset).toHaveBeenCalled();
  });

  it('a focus-switch show (reason:"focus") changes nothing — same visit', async () => {
    const reset = vi.fn();
    (window as { padnav?: unknown }).padnav = { reset };
    const bridge = makeBridge();
    const el = await mount(bridge);
    bridge.emit('settings.data', WIDGETS);
    bridge.emit('views.data', VIEWS);
    await flush();

    selectRail(el, 'Acme Kit');
    await flush();
    el.querySelector<HTMLButtonElement>('#ctl-acme\\.kit-boolOn')!.click();
    await flush();
    expect(el.querySelector<HTMLButtonElement>('#session-chip')!.style.display).not.toBe('none');

    bridge.emit('ui.visibility', { visible: false, reason: 'focus' });
    bridge.emit('ui.visibility', { visible: true, reason: 'focus' });
    await flush();

    expect(el.querySelector('.detail-head h2')!.textContent).toBe('Acme Kit');
    expect(el.querySelector<HTMLButtonElement>('#session-chip')!.style.display).not.toBe('none');
    expect(reset).not.toHaveBeenCalled();
  });

  it('a hide edge (visible:false) changes nothing', async () => {
    const reset = vi.fn();
    (window as { padnav?: unknown }).padnav = { reset };
    const bridge = makeBridge();
    const el = await mount(bridge);
    bridge.emit('settings.data', WIDGETS);
    bridge.emit('views.data', VIEWS);
    await flush();

    selectRail(el, 'Acme Kit');
    await flush();
    expect(el.querySelector('.detail-head h2')!.textContent).toBe('Acme Kit');

    bridge.emit('ui.visibility', { visible: false });
    await flush();
    // Selection retained, padnav untouched.
    expect(el.querySelector('.detail-head h2')!.textContent).toBe('Acme Kit');
    expect(reset).not.toHaveBeenCalled();
  });
});
