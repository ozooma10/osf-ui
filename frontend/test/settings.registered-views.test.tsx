// @vitest-environment jsdom
//
// The framework diagnostics group is the escape hatch for every discovered
// view, including entries deliberately omitted from normal navigation.

import { afterEach, describe, expect, it } from 'vitest';
import { flush, makeBridge, mount, unmount } from './helpers/settingsHarness';

afterEach(unmount);

describe('registered views diagnostics', () => {
  it('lists the unfiltered discovery catalog and triggers the normal open path', async () => {
    const bridge = makeBridge();
    const el = await mount(bridge);
    bridge.deliver('settings.data', {
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
    bridge.deliver('views.data', {
      views: [
        {
          id: 'osfui/settings',
          title: 'Mod Settings',
          kind: 'menu',
          hub: true,
          loadState: 'loaded',
        },
        {
          id: 'tools/hidden-lab',
          title: 'Hidden Lab',
          kind: 'menu',
          hub: false,
          loadState: 'unloaded',
        },
        {
          id: 'tools/passive-hud',
          title: 'Passive HUD',
          kind: 'hud',
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

    const rows = [...el.querySelectorAll<HTMLElement>('.registered-view')];
    expect(rows.map((row) => row.querySelector('.registered-view-id')!.textContent)).toEqual([
      'osfui/settings',
      'tools/hidden-lab',
      'tools/passive-hud',
    ]);
    expect(el.querySelector('.detail')!.textContent).toContain('unloaded');

    rows[1]!.querySelector<HTMLButtonElement>('button')!.click();
    expect(bridge.sent[bridge.sent.length - 1]).toEqual({
      command: 'menu.open',
      fields: { view: 'tools/hidden-lab' },
    });
  });
});