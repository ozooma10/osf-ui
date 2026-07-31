// @vitest-environment jsdom
//
// The framework diagnostics group is the escape hatch for every mod-provided
// view, including entries deliberately omitted from normal navigation.

import { afterEach, describe, expect, it } from 'vitest';
import { flush, makeBridge, mount, unmount } from './helpers/settingsHarness';

afterEach(unmount);

describe('registered views diagnostics', () => {
  it('lists mod-provided discovery entries and triggers the normal open path', async () => {
    const bridge = makeBridge();
    const el = await mount(bridge);
    bridge.publish('osfui/settings', {
      mods: [
        {
          id: 'osfui',
          title: 'OSF UI',
          values: { renderStats: false },
          schema: {
            groups: [
              {
                id: 'diagnostics',
                label: 'Diagnostics',
                settings: [
                  {
                    key: 'renderStats',
                    label: 'Show render stats',
                    type: 'bool',
                    default: false,
                  },
                ],
              },
            ],
          },
        },
      ],
    });
    bridge.publish('osfui/views', {
      views: [
        {
          id: 'osfui/settings',
          title: 'Mod Settings',
          kind: 'menu',
          mod: 'osfui',
          hub: true,
          loadState: 'loaded',
        },
        {
          id: 'tools/hidden-lab',
          title: 'Hidden Lab',
          kind: 'menu',
          mod: 'example.tools',
          hub: false,
          loadState: 'unloaded',
        },
        {
          id: 'tools/passive-hud',
          title: 'Passive HUD',
          kind: 'hud',
          mod: 'example.tools',
          hub: false,
          loadState: 'loaded',
        },
      ],
    });
    await flush();

    [...el.querySelectorAll<HTMLButtonElement>('.rail-item')]
      .find((button) => button.textContent!.includes('OSF UI'))!
      .click();
    await flush();

    const diagnostics = [...el.querySelectorAll<HTMLElement>('.group')].find((group) =>
      group.querySelector('.group-label')?.textContent?.includes('Diagnostics'),
    )!;
    expect(diagnostics.classList.contains('collapsed')).toBe(false);

    const toggle = diagnostics.querySelector<HTMLButtonElement>('.registered-views-head')!;
    expect(toggle.getAttribute('aria-expanded')).toBe('false');
    expect(diagnostics.querySelector('.registered-view')).toBeNull();
    toggle.click();
    await flush();
    expect(toggle.getAttribute('aria-expanded')).toBe('true');

    const rows = [...el.querySelectorAll<HTMLElement>('.registered-view')];
    expect(rows.map((row) => row.querySelector('.registered-view-id')!.textContent)).toEqual([
      'tools/hidden-lab',
      'tools/passive-hud',
    ]);
    expect(el.querySelector('.detail')!.textContent).toContain('unloaded');

    rows[0]!.querySelector<HTMLButtonElement>('button')!.click();
    expect(bridge.outbound[bridge.outbound.length - 1]).toEqual({
      name: 'menu.open',
      payload: { view: 'tools/hidden-lab' },
    });

    // Idle reclaim is a live loaded -> unloaded catalog transition. The row
    // must stay present and openable rather than being treated as a removal.
    bridge.publish('osfui/views', {
      views: [
        {
          id: 'osfui/settings',
          title: 'Mod Settings',
          kind: 'menu',
          mod: 'osfui',
          hub: true,
          loadState: 'loaded',
        },
        {
          id: 'tools/hidden-lab',
          title: 'Hidden Lab',
          kind: 'menu',
          mod: 'example.tools',
          hub: false,
          loadState: 'unloaded',
        },
        {
          id: 'tools/passive-hud',
          title: 'Passive HUD',
          kind: 'hud',
          mod: 'example.tools',
          hub: false,
          loadState: 'unloaded',
        },
      ],
    });
    await flush();
    const reclaimed = [...el.querySelectorAll<HTMLElement>('.registered-view')]
      .find((row) => row.textContent!.includes('tools/passive-hud'))!;
    expect(reclaimed.textContent).toContain('unloaded');
    expect(reclaimed.querySelector<HTMLButtonElement>('button')!.disabled).toBe(false);
  });
});
