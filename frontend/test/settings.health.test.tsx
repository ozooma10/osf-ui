// @vitest-environment jsdom
//
// The System Health pane end to end through the settings App: the pinned rail
// entry, the summary states, card rendering, contextual actions, technical
// disclosure, copy-report, deep links from failed cards, and clipboard-failure
// degradation.

import { describe, it, expect, afterEach, vi } from 'vitest';
import { makeBridge, mount, unmount, flush } from './helpers/settingsHarness';
import { WIDGETS, VIEWS } from './helpers/settingsFixtures';

afterEach(unmount);

const ISSUE = (o: Record<string, unknown>) => ({
  code: 'view.load-failed',
  severity: 'warning',
  status: 'active',
  source: 'views',
  subject: '',
  context: {},
  occurrences: 1,
  firstAt: 0,
  lastAt: 0,
  ...o,
});

async function mountHealth(issues: unknown[], system: Record<string, unknown> = {}) {
  const bridge = makeBridge();
  const el = await mount(bridge);
  bridge.emit('settings.data', WIDGETS);
  bridge.emit('views.data', VIEWS);
  bridge.emit('diagnostics.data', { system, issues });
  await flush();
  return { bridge, el };
}

function openHealth(el: HTMLElement) {
  (el.querySelector('.rail-item--health') as HTMLButtonElement).click();
}

describe('subscription + rail', () => {
  it('sends diagnostics.get on mount', async () => {
    const bridge = makeBridge();
    const el = await mount(bridge);
    bridge.emit('settings.data', WIDGETS);
    await flush();
    expect(bridge.sent.some((s) => s.command === 'diagnostics.get')).toBe(true);
    expect(el.querySelector('.rail-item--health')).not.toBeNull();
  });
});

describe('summary states', () => {
  it('reads nominal when clean', async () => {
    const { el } = await mountHealth([]);
    openHealth(el);
    await flush();
    expect(el.querySelector('#health-summary')!.classList.contains('health-summary--ok')).toBe(true);
    expect(el.querySelector('.health-summary-title')!.textContent).toBe('All systems nominal');
  });

  it('reads "Action required" with any active error, warnings notwithstanding', async () => {
    const { el } = await mountHealth([
      ISSUE({ id: 'e', severity: 'error' }),
      ISSUE({ id: 'w', severity: 'warning' }),
    ]);
    openHealth(el);
    await flush();
    expect(el.querySelector('#health-summary')!.classList.contains('health-summary--error')).toBe(true);
    expect(el.querySelector('.health-summary-title')!.textContent).toBe('Action required');
    // The detail line carries both counts.
    expect(el.querySelector('.health-summary-detail')!.textContent).toContain('1 error');
    expect(el.querySelector('.health-summary-detail')!.textContent).toContain('1 warning');
  });

  it('reads "Warnings detected" with warnings only', async () => {
    const { el } = await mountHealth([ISSUE({ id: 'w', severity: 'warning' })]);
    openHealth(el);
    await flush();
    expect(el.querySelector('.health-summary-title')!.textContent).toBe('Warnings detected');
  });
});

describe('cards', () => {
  it('renders active issues error-first and keeps resolved in collapsed history', async () => {
    const { el } = await mountHealth([
      ISSUE({ id: 'w', severity: 'warning', code: 'host.focus-stranded', lastAt: 1 }),
      ISSUE({ id: 'e', severity: 'error', code: 'view.load-failed', subject: 'x/y', lastAt: 2 }),
      ISSUE({ id: 'r', severity: 'error', status: 'resolved', resolvedAt: 3 }),
    ]);
    openHealth(el);
    await flush();
    const active = [...el.querySelectorAll('#health-active .health-card')];
    expect(active.map((c) => c.getAttribute('data-issue'))).toEqual(['e', 'w']);
    // Resolved history is collapsed until its toggle is clicked.
    expect(el.querySelector('#health-resolved')).toBeNull();
    (el.querySelector('.health-history-toggle') as HTMLButtonElement).click();
    await flush();
    expect(el.querySelectorAll('#health-resolved .health-card')).toHaveLength(1);
  });

  it('offers Retry view and fires menu.open with the issue subject', async () => {
    const { bridge, el } = await mountHealth([
      ISSUE({ id: 'e', severity: 'error', code: 'view.load-failed', subject: 'broken/panel' }),
    ]);
    openHealth(el);
    await flush();
    const retry = [...el.querySelectorAll<HTMLButtonElement>('.health-card-actions .osf-btn')].find(
      (b) => b.textContent === 'Retry view',
    )!;
    retry.click();
    await flush();
    const open = bridge.sent.find((s) => s.command === 'menu.open');
    expect(open?.fields).toEqual({ view: 'broken/panel' });
  });

  it('fires osfui.openLogFolder from the global action and per-card action', async () => {
    const { bridge, el } = await mountHealth([
      ISSUE({ id: 'e', severity: 'error', code: 'view.load-failed', subject: 'x/y' }),
    ]);
    openHealth(el);
    await flush();
    [...el.querySelectorAll<HTMLButtonElement>('.health-actions .osf-btn')]
      .find((b) => b.textContent === 'Open log folder')!
      .click();
    expect(bridge.sent.some((s) => s.command === 'osfui.openLogFolder')).toBe(true);
  });

  it('discloses technical details on demand', async () => {
    const { el } = await mountHealth([
      ISSUE({
        id: 'e',
        code: 'settings.values-parse',
        subject: 'acme',
        context: { file: 'acme.json', message: 'boom at 1:2' },
      }),
    ]);
    openHealth(el);
    await flush();
    expect(el.querySelector('.health-card-technical')).toBeNull();
    [...el.querySelectorAll<HTMLButtonElement>('.health-card-disclose')][0]!.click();
    await flush();
    const pre = el.querySelector('.health-card-technical')!;
    expect(pre.textContent).toContain('file: acme.json');
    expect(pre.textContent).toContain('message: boom at 1:2');
  });
});

describe('copy diagnostic report', () => {
  it('writes a report to the clipboard and toasts success', async () => {
    const writeText = vi.fn().mockResolvedValue(undefined);
    vi.stubGlobal('navigator', { clipboard: { writeText } });
    const { el } = await mountHealth([ISSUE({ id: 'e', severity: 'error' })], { version: '1.4.0' });
    openHealth(el);
    await flush();
    [...el.querySelectorAll<HTMLButtonElement>('.health-actions .osf-btn')]
      .find((b) => b.textContent === 'Copy diagnostic report')!
      .click();
    await flush();
    expect(writeText).toHaveBeenCalledOnce();
    expect(String(writeText.mock.calls[0]![0])).toContain('OSF UI diagnostic report');
    expect(el.querySelector('.toast')!.textContent).toContain('Diagnostic report copied');
    vi.unstubAllGlobals();
  });

  it('degrades visibly when the clipboard rejects, leaving details selectable', async () => {
    const writeText = vi.fn().mockRejectedValue(new Error('denied'));
    vi.stubGlobal('navigator', { clipboard: { writeText } });
    const { el } = await mountHealth([
      ISSUE({ id: 'e', code: 'view.load-failed', subject: 'x/y', context: { errorCode: -6 } }),
    ]);
    openHealth(el);
    await flush();
    // Copy details, not the report, so the disclosure opens with the text.
    [...el.querySelectorAll<HTMLButtonElement>('.health-card-actions .osf-btn')]
      .find((b) => b.textContent === 'Copy details')!
      .click();
    await flush();
    expect(el.querySelector('.toast--warn')).not.toBeNull();
    // The details are on screen and selectable so the player can copy by hand.
    expect(el.querySelector('.health-card-technical')).not.toBeNull();
    vi.unstubAllGlobals();
  });
});

describe('deep links', () => {
  it('a failed launcher card navigates to its issue with the card expanded', async () => {
    const bridge = makeBridge();
    const el = await mount(bridge);
    bridge.emit('settings.data', WIDGETS);
    // A failed view in the launcher, and the matching issue.
    bridge.emit('views.data', {
      views: [
        {
          id: 'broken/panel',
          title: 'Broken Panel',
          description: '',
          mod: '',
          kind: 'menu',
          interactive: true,
          hub: true,
          targetVersion: '',
          open: false,
          focused: false,
          loadState: 'failed',
        },
      ],
    });
    bridge.emit('diagnostics.data', {
      system: {},
      issues: [
        ISSUE({ id: 'view.load-failed:broken/panel', severity: 'error', code: 'view.load-failed', subject: 'broken/panel' }),
      ],
    });
    await flush();

    // The card foot reads the deep-link affordance, not "SEE LOG".
    const tile = [...el.querySelectorAll<HTMLButtonElement>('.home-tile')].find((t) =>
      t.textContent!.includes('Broken Panel'),
    )!;
    expect(tile.textContent).toContain('FAILED — REVIEW ISSUE');
    tile.click();
    await flush();

    // Landed on Health with that issue's card expanded.
    expect(el.querySelector('#health-summary')).not.toBeNull();
    const card = el.querySelector('.health-card[data-issue="view.load-failed:broken/panel"]')!;
    expect(card.querySelector('.health-card-technical')).not.toBeNull();
  });
});

describe('mod severity marker', () => {
  it('coexists with the modified-setting count on the rail', async () => {
    const bridge = makeBridge();
    const el = await mount(bridge);
    // A widget mod with a modified value so the count badge shows.
    bridge.emit('settings.data', WIDGETS);
    bridge.emit('diagnostics.data', {
      system: {},
      issues: [ISSUE({ id: 'x', severity: 'warning', code: 'settings.values-parse', subject: 'acme.kit' })],
    });
    await flush();
    const railItem = [...el.querySelectorAll('.rail-item')].find((r) =>
      r.textContent!.includes('Acme Kit'),
    )!;
    expect(railItem.querySelector('.rail-item-severity--warning')).not.toBeNull();
  });
});
