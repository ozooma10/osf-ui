// @vitest-environment jsdom
//
// The pane's five detail modes in dispatch order, the section index threshold,
// search-jump, and rail cycling.

import { describe, it, expect, afterEach } from 'vitest';
import { makeBridge, mount, unmount, flush, typeFilter } from './helpers/settingsHarness';
import { WIDGETS, VIEWS, MANY_GROUPS, FOUR_GROUPS } from './helpers/settingsFixtures';

afterEach(unmount);

async function mountWith(data: unknown, views?: unknown) {
  const bridge = makeBridge();
  const el = await mount(bridge);
  bridge.emit('settings.data', data);
  if (views) bridge.emit('views.data', views);
  await flush();
  return { bridge, el };
}

function selectRail(el: HTMLElement, label: string) {
  [...el.querySelectorAll<HTMLButtonElement>('.rail-item')]
    .find((b) => b.textContent!.includes(label))!
    .click();
}

describe('detail modes', () => {
  it('lands on Home (the launcher) by default', async () => {
    const { el } = await mountWith(WIDGETS, VIEWS);
    expect(el.querySelector('.detail-body--home')).not.toBeNull();
    expect(el.querySelector('.home-grid')).not.toBeNull();
    expect(el.querySelector('.detail-head h2')!.textContent).toBe('All systems');
  });

  it('mode 1 (search) WINS over Home: a non-empty filter shows results, not cards', async () => {
    const { el } = await mountWith(WIDGETS, VIEWS);
    await typeFilter(el, 'slider');
    expect(el.querySelector('.search-results')).not.toBeNull();
    expect(el.querySelector('.home-grid')).toBeNull();
    expect(el.querySelector('.detail-head h2')!.textContent).toContain('slider');
  });

  it('mode 3 (not found): nothing installed still pins System Health', async () => {
    // No mods and no views: the rail collapses to its pinned destinations, so
    // System Health is always reachable — it is where load failures now live.
    const { el } = await mountWith({ mods: [] });
    expect(el.querySelector('.rail-item--health')).not.toBeNull();
  });

  it('mode 4 (view-only): a views-but-no-schema entry shows "registers no settings"', async () => {
    const { el } = await mountWith(WIDGETS, VIEWS);
    selectRail(el, 'Standalone Browser');
    await flush();
    expect(el.querySelector('.detail-quiet')!.textContent).toBe('This mod registers no settings.');
    // Surfaces section, but no Reset all button.
    expect(el.querySelector('.group')).not.toBeNull();
    expect([...el.querySelectorAll('.osf-btn')].some((b) => b.textContent === 'Reset all')).toBe(false);
  });

  it('mode 5 (settings page): a settings mod shows its head, reset-all and groups', async () => {
    const { el } = await mountWith(WIDGETS, VIEWS);
    selectRail(el, 'Acme Kit');
    await flush();
    expect(el.querySelector('.detail-head h2')!.textContent).toBe('Acme Kit');
    expect([...el.querySelectorAll('.osf-btn')].some((b) => b.textContent === 'Reset all')).toBe(true);
    expect(el.querySelectorAll('.group').length).toBeGreaterThan(1);
  });
});

describe('section index', () => {
  it('appears only with MORE THAN 4 labelled groups', async () => {
    const { el } = await mountWith(MANY_GROUPS);
    selectRail(el, 'Big Mod');
    await flush();
    const index = el.querySelector('.section-index');
    expect(index).not.toBeNull();
    expect(index!.querySelectorAll('.section-index-item').length).toBe(5);
  });

  it('does NOT appear at exactly 4 labelled groups', async () => {
    const { el } = await mountWith(FOUR_GROUPS);
    selectRail(el, 'Four Mod');
    await flush();
    expect(el.querySelector('.section-index')).toBeNull();
  });

  it('group anchor slug is grp-<label lowercased, spaces to dashes>', async () => {
    const { el } = await mountWith(
      {
        mods: [
          {
            id: 'g',
            title: 'G',
            values: {},
            schema: {
              groups: [
                { label: 'Two Words', settings: [{ key: 'a', label: 'A', type: 'bool', default: false }] },
              ],
            },
          },
        ],
      },
      undefined,
    );
    selectRail(el, 'G');
    await flush();
    expect(el.querySelector('#grp-two-words')).not.toBeNull();
  });
});

describe('search jump', () => {
  it('clears the filter, selects the owner and flashes the target row', async () => {
    const { el } = await mountWith(WIDGETS, VIEWS);
    await typeFilter(el, 'slider');

    const input = el.querySelector<HTMLInputElement>('#filter')!;
    const result = el.querySelector<HTMLButtonElement>('.search-result')!;
    result.click();
    await flush();

    // Filter cleared -> back to the settings page.
    expect(input.value).toBe('');
    expect(el.querySelector('.search-results')).toBeNull();
    const row = el.querySelector('.row[data-key="slide"]')!;
    expect(row.classList.contains('flash')).toBe(true);
  });
});

describe('rail cycling (LB/RB)', () => {
  it('RB steps the selection forward in painted order', async () => {
    const { bridge, el } = await mountWith(WIDGETS, VIEWS);
    // Painted order: Home, framework (osfui), then title-sorted mods.
    expect(el.querySelector('.rail-item--home.selected')).not.toBeNull();
    bridge.emit('ui.gamepad', { kind: 'button', button: { id: 0x0200, down: true } });
    await flush();
    const selected = el.querySelector('.rail-item.selected')!;
    expect(selected.textContent).toContain('OSF UI');
  });

  it('a modal (undo panel) swallows shoulder presses', async () => {
    const { bridge, el } = await mountWith(WIDGETS, VIEWS);
    selectRail(el, 'Acme Kit');
    await flush();
    // Make a change so the undo chip appears, then open it.
    el.querySelector<HTMLButtonElement>('#ctl-acme\\.kit-boolOn')!.click();
    await flush();
    el.querySelector<HTMLButtonElement>('#session-chip')!.click();
    await flush();
    expect(el.querySelector('.session-overlay')).not.toBeNull();

    const before = el.querySelector('.rail-item.selected')!.textContent;
    bridge.emit('ui.gamepad', { kind: 'button', button: { id: 0x0200, down: true } });
    await flush();
    expect(el.querySelector('.rail-item.selected')!.textContent).toBe(before);
  });
});

describe('System Health rail entry', () => {
  it('is pinned even while a filter matches nothing', async () => {
    // The reason the old load-error alert was never filtered: a player typing
    // the name of a broken mod must still reach the explanation.
    const { el } = await mountWith({ mods: [] });
    await typeFilter(el, 'anything');
    expect(el.querySelector('.rail-item--health')).not.toBeNull();
  });

  it('reflects diagnostics severity in its badge and clears search when selected', async () => {
    const { bridge, el } = await mountWith(WIDGETS, VIEWS);
    bridge.emit('diagnostics.data', {
      system: {},
      issues: [
        {
          id: 'view.load-failed:x/y',
          code: 'view.load-failed',
          severity: 'error',
          status: 'active',
          source: 'views',
          subject: 'x/y',
          context: {},
          occurrences: 1,
          firstAt: 1,
          lastAt: 1,
        },
      ],
    });
    await flush();
    const health = el.querySelector('.rail-item--health')!;
    expect(health.classList.contains('rail-item--health-error')).toBe(true);
    expect(health.querySelector('.rail-item-count--error')!.textContent).toBe('1');

    // Selecting Health from a filtered rail clears the search and shows the pane.
    await typeFilter(el, 'zzz');
    (health as HTMLButtonElement).click();
    await flush();
    expect(el.querySelector('#health-summary')).not.toBeNull();
    expect(el.querySelector<HTMLInputElement>('#filter')!.value).toBe('');
  });
});
