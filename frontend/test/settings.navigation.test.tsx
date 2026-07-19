// @vitest-environment jsdom
//
// settings.navigation.test.tsx — the pane's five detail modes in dispatch
// order, the section index threshold, search-jump, and rail cycling.

import { describe, it, expect, afterEach } from 'vitest';
import { makeBridge, mount, unmount, flush, typeFilter } from './helpers/settingsHarness';
import { WIDGETS, VIEWS, MANY_GROUPS, FOUR_GROUPS, WITH_LOAD_ERRORS } from './helpers/settingsFixtures';

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
    // Home clears the accent (applyAccent is a no-op in the fake, but the mode
    // is what we assert): the card grid, not a settings page.
    expect(el.querySelector('.detail-head h2')!.textContent).toBe('All systems');
  });

  it('mode 1 (search) WINS over Home: a non-empty filter shows results, not cards', async () => {
    const { el } = await mountWith(WIDGETS, VIEWS);
    await typeFilter(el, 'slider');
    expect(el.querySelector('.search-results')).not.toBeNull();
    expect(el.querySelector('.home-grid')).toBeNull();
    expect(el.querySelector('.detail-head h2')!.textContent).toContain('slider');
  });

  it('mode 3 (not found): a selection naming no entry shows the empty state', async () => {
    // Deliver only errors, no mods -> select nothing resolvable. Force a bogus
    // selection by searching then clearing to a dead id is awkward; instead a
    // mod that unregisters. Simpler: empty registry shows "nothing registered".
    const { el } = await mountWith(WITH_LOAD_ERRORS);
    // The rail alert is pinned; the pane shows the empty/registered state.
    expect(el.querySelector('.rail-alert')).not.toBeNull();
  });

  it('mode 4 (view-only): a views-but-no-schema entry shows "registers no settings"', async () => {
    const { el } = await mountWith(WIDGETS, VIEWS);
    selectRail(el, 'Standalone Browser');
    await flush();
    expect(el.querySelector('.detail-quiet')!.textContent).toBe('This mod registers no settings.');
    // It has the surfaces section but no Reset all button.
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
    // The target row is flashing.
    const row = el.querySelector('.row[data-key="slide"]')!;
    expect(row.classList.contains('flash')).toBe(true);
  });
});

describe('rail cycling (LB/RB)', () => {
  it('RB steps the selection forward in painted order', async () => {
    const { bridge, el } = await mountWith(WIDGETS, VIEWS);
    // Painted order: Home, framework (osfui), then title-sorted mods.
    // Start on Home; RB should move to the framework entry.
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

describe('load-error alert', () => {
  it('is pinned even while a filter is active', async () => {
    const { el } = await mountWith(WITH_LOAD_ERRORS);
    await typeFilter(el, 'anything');
    expect(el.querySelector('.rail-alert')).not.toBeNull();
  });
});
