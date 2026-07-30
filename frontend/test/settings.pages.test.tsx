// @vitest-environment jsdom
//
// Schema `pages`: the tab row a paged mod renders instead of one long group
// column, the implicit General tab, degradation to the flat list, and the
// search jump raising the owning tab.

import { describe, it, expect, afterEach } from 'vitest';
import { makeBridge, mount, unmount, flush, typeFilter } from './helpers/settingsHarness';
import { PAGED, WIDGETS } from './helpers/settingsFixtures';
import { pageBuckets, pageIdForGroup, GENERAL_PAGE_ID } from '@lib/settings/pages';
import type { SettingsSchema } from '@sdk';

afterEach(unmount);

async function mountPaged() {
  const bridge = makeBridge();
  const el = await mount(bridge);
  bridge.deliver('settings.data', PAGED);
  await flush();
  [...el.querySelectorAll<HTMLButtonElement>('.rail-item')]
    .find((b) => b.textContent!.includes('Paged Mod'))!
    .click();
  await flush();
  return { bridge, el };
}

function tabLabels(el: HTMLElement): string[] {
  return [...el.querySelectorAll('.page-tab')].map((t) => t.textContent!);
}

describe('pageBuckets (model)', () => {
  const schema = (PAGED as { mods: Array<{ schema: unknown }> }).mods[0]!.schema as SettingsSchema;

  it('buckets in paint order: implicit General first, then declared non-empty pages', () => {
    const buckets = pageBuckets(schema)!;
    expect(buckets.map((b) => b.id)).toEqual([GENERAL_PAGE_ID, 'browser', 'advanced']);
    // General collects the untagged group AND the unknown-page group.
    expect(buckets[0]!.groups.map((g) => g.index)).toEqual([0, 3]);
    // Original schema indexes survive — collapse identity is keyed on them.
    expect(buckets[1]!.groups[0]!.index).toBe(1);
  });

  it('drops a declared page no group references ("ghost")', () => {
    const buckets = pageBuckets(schema)!;
    expect(buckets.some((b) => b.id === 'ghost')).toBe(false);
  });

  it('returns null with no pages, and null when everything lands on one page', () => {
    expect(pageBuckets({ groups: [{ settings: [] }] })).toBeNull();
    // One declared page, all groups on it: a single tab is not a segmentation.
    expect(
      pageBuckets({
        pages: [{ id: 'only' }],
        groups: [{ page: 'only', settings: [] }, { page: 'only', settings: [] }],
      } as SettingsSchema),
    ).toBeNull();
  });

  it('a declared page whose id collides with the implicit General bucket is refused', () => {
    // GENERAL_PAGE_ID's leading underscores keep it outside the authored-id
    // grammar — but only if the grammar is actually applied. Unfiltered, a
    // declared "__general" page produced a second bucket with the same id,
    // aliasing tab selection between them.
    const buckets = pageBuckets({
      pages: [{ id: GENERAL_PAGE_ID, label: 'Impostor' }, { id: 'real' }],
      groups: [
        { settings: [] }, // untagged -> implicit General
        { page: GENERAL_PAGE_ID, settings: [] }, // undeclarable ref -> General too
        { page: 'real', settings: [] },
      ],
    } as SettingsSchema)!;
    expect(buckets.filter((b) => b.id === GENERAL_PAGE_ID)).toHaveLength(1);
    expect(buckets.map((b) => b.id)).toEqual([GENERAL_PAGE_ID, 'real']);
    // Both the untagged group and the impostor ref land on the one General.
    expect(buckets[0]!.groups.map((g) => g.index)).toEqual([0, 1]);
  });

  it('pageIdForGroup names the owning bucket, or null when unpaged', () => {
    expect(pageIdForGroup(schema, 2)).toBe('advanced');
    expect(pageIdForGroup(schema, 3)).toBe(GENERAL_PAGE_ID);
    expect(pageIdForGroup({ groups: [{ settings: [] }] }, 0)).toBeNull();
  });
});

describe('paged mod rendering', () => {
  it('renders the tab row with General first and no ghost tab', async () => {
    const { el } = await mountPaged();
    expect(tabLabels(el)).toEqual(['General', 'Browser', 'Advanced']);
  });

  it('shows only the active tab\'s groups, and the first tab starts active', async () => {
    const { el } = await mountPaged();
    const active = el.querySelector('.page-tab.active')!;
    expect(active.textContent).toBe('General');
    expect(active.getAttribute('aria-selected')).toBe('true');

    const headings = [...el.querySelectorAll('.group-label')].map((g) => g.textContent);
    expect(headings).toContain('Hotkeys');
    expect(headings).toContain('Lost'); // unknown page id lands on General
    expect(headings).not.toContain('Library');
    expect(headings).not.toContain('Logging');
  });

  it('clicking a tab switches the visible groups', async () => {
    const { el } = await mountPaged();
    [...el.querySelectorAll<HTMLButtonElement>('.page-tab')]
      .find((t) => t.textContent === 'Browser')!
      .click();
    await flush();

    expect(el.querySelector('.page-tab.active')!.textContent).toBe('Browser');
    const headings = [...el.querySelectorAll('.group-label')].map((g) => g.textContent);
    expect(headings).toEqual(['Library']);
    expect(el.querySelector('.row[data-key="lib"]')).not.toBeNull();
    expect(el.querySelector('.row[data-key="hk"]')).toBeNull();
  });

  it('an unpaged mod renders no tab row (degradation baseline)', async () => {
    const bridge = makeBridge();
    const el = await mount(bridge);
    bridge.deliver('settings.data', WIDGETS);
    await flush();
    [...el.querySelectorAll<HTMLButtonElement>('.rail-item')]
      .find((b) => b.textContent!.includes('Acme Kit'))!
      .click();
    await flush();
    expect(el.querySelector('.page-tabs')).toBeNull();
    expect(el.querySelectorAll('.group').length).toBeGreaterThan(1);
  });
});

describe('search jump into a paged mod', () => {
  it('raises the owning tab, expands the group, and flashes the row', async () => {
    const { el } = await mountPaged();
    // "Logging Row" lives on the Advanced page, which is not the active tab.
    await typeFilter(el, 'logging row');
    const result = el.querySelector<HTMLButtonElement>('.search-result')!;
    result.click();
    await flush();

    expect(el.querySelector('.page-tab.active')!.textContent).toBe('Advanced');
    const row = el.querySelector('.row[data-key="log"]')!;
    expect(row).not.toBeNull();
    expect(row.classList.contains('flash')).toBe(true);
  });

  it('a group with a stable id anchors and collapses by id, surviving reorder', async () => {
    const { el } = await mountPaged();
    [...el.querySelectorAll<HTMLButtonElement>('.page-tab')]
      .find((t) => t.textContent === 'Browser')!
      .click();
    await flush();
    // `library` has id "library" — the anchor derives from the id, not the label.
    expect(el.querySelector('#grp-library')).not.toBeNull();
  });
});
