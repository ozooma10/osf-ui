// @vitest-environment jsdom

import { afterEach, describe, expect, it } from 'vitest';
import { flush, makeBridge, mount, unmount } from './helpers/settingsHarness';

afterEach(unmount);

describe('discovered views inventory', () => {
  it('lists mod-provided discovery entries and triggers the normal open path', async () => {
    const bridge = makeBridge();
    const el = await mount(bridge);
    bridge.publish('osfui/settings', {
      mods: [
        {
          id: 'osfui',
          title: 'OSF UI',
          values: { healthDetail: 'compact' },
          schema: {
            groups: [
              {
                id: 'interface',
                label: 'Interface',
                settings: [
                  {
                    key: 'healthDetail',
                    label: 'Health detail',
                    type: 'string',
                    default: 'compact',
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
          id: 'example.tools/hidden-lab',
          title: 'Hidden Lab',
          kind: 'menu',
          mod: 'example.tools',
          hub: false,
          loadState: 'unloaded',
        },
        {
          id: 'example.tools/passive-hud',
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

    const interfaceGroup = [...el.querySelectorAll<HTMLElement>('.group')].find((group) =>
      group.querySelector('.group-label')?.textContent?.includes('Interface'),
    )!;
    expect(interfaceGroup.classList.contains('collapsed')).toBe(false);

    const inventory = el.querySelector<HTMLElement>(
      '.discovered-views-group .group-rows > .discovered-views',
    )!;
    expect(inventory).not.toBeNull();
    const toggle = inventory.querySelector<HTMLButtonElement>('.discovered-views-head')!;
    expect(toggle.getAttribute('aria-expanded')).toBe('false');
    expect(el.querySelector('.discovered-view')).toBeNull();
    toggle.click();
    await flush();
    expect(toggle.getAttribute('aria-expanded')).toBe('true');

    const rows = [...el.querySelectorAll<HTMLElement>('.discovered-view')];
    expect(rows.map((row) => row.querySelector('.discovered-view-id')!.textContent)).toEqual([
      'example.tools/hidden-lab',
      'example.tools/passive-hud',
    ]);
    expect(el.querySelector('.detail')!.textContent).toContain('unloaded');

    rows[0]!.querySelector<HTMLButtonElement>('button')!.click();
    expect(bridge.outbound[bridge.outbound.length - 1]).toEqual({
      name: 'menu.open',
      payload: { view: 'example.tools/hidden-lab' },
    });
  });
});
