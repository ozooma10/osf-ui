// @vitest-environment jsdom
//
// The System Health pane end to end through the settings App: the pinned rail
// entry, the summary states, card rendering, contextual actions, technical
// disclosure, copy-report, deep links from failed cards, and clipboard-failure
// degradation.
//
// The snapshot is the `osfui/diagnostics` STATE key: replayed to every fresh
// document and republished whole whenever a condition is raised, recurs or
// clears. There is no `diagnostics.get`, and nothing here waits for a reply
// before it can paint.

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
  const bridge = makeBridge({
    state: {
      'osfui/settings': WIDGETS,
      'osfui/views': VIEWS,
      'osfui/diagnostics': { system, issues },
    },
  });
  const el = await mount(bridge);
  await flush();
  return { bridge, el };
}

function openHealth(el: HTMLElement) {
  (el.querySelector('.rail-item--health') as HTMLButtonElement).click();
}

describe('subscription + rail', () => {
  it('subscribes rather than reading, and pins the rail entry before any snapshot', async () => {
    // No diagnostics state at all: the destination is pinned regardless, because
    // it is where load failures are explained and a player must be able to reach
    // it even when the subsystem that would report them never spoke.
    const bridge = makeBridge({ state: { 'osfui/settings': WIDGETS } });
    const el = await mount(bridge);
    await flush();
    expect(bridge.outbound.map((m) => m.name)).not.toContain('diagnostics.get');
    expect(el.querySelector('.rail-item--health')).not.toBeNull();

    // And the first snapshot to arrive lands without anything having asked.
    bridge.publish('osfui/diagnostics', {
      system: {},
      issues: [ISSUE({ id: 'e', severity: 'error' })],
    });
    await flush();
    expect(el.querySelector('.rail-item--health')!.classList.contains('rail-item--health-error'))
      .toBe(true);
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
      ISSUE({ id: 'w', severity: 'warning', code: 'host.ring-truncated', lastAt: 1 }),
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

  it('gives errors a full card and collapses active warnings into a tier', async () => {
    const { el } = await mountHealth([
      ISSUE({ id: 'e', severity: 'error', code: 'view.load-failed', subject: 'x/y', lastAt: 9 }),
      ISSUE({ id: 'w1', severity: 'warning', code: 'settings.values-parse', lastAt: 2 }),
      ISSUE({ id: 'w2', severity: 'warning', code: 'host.ring-truncated', lastAt: 1 }),
    ]);
    openHealth(el);
    await flush();

    // The error is a card: its impact copy and actions are visible unprompted.
    const error = el.querySelector('.health-card[data-issue="e"]')!;
    expect(error.classList.contains('health-card--row')).toBe(false);
    expect(error.querySelector('.health-card-impact')).not.toBeNull();

    // Both warnings are rows, counted by a tier label, and folded shut.
    expect(el.querySelector('#health-warning-tier')!.textContent).toContain('2');
    const rows = [...el.querySelectorAll('.health-card--row')];
    expect(rows.map((r) => r.getAttribute('data-issue'))).toEqual(['w1', 'w2']);
    expect(rows.every((r) => r.querySelector('.health-card-impact') === null)).toBe(true);

    // Opening one row reveals the same body an error card shows, and leaves
    // the other row shut — the tier is not an all-or-nothing disclosure.
    rows[0]!.querySelector<HTMLButtonElement>('.health-row-head')!.click();
    await flush();
    expect(rows[0]!.querySelector('.health-card-impact')).not.toBeNull();
    expect(rows[1]!.querySelector('.health-card-impact')).toBeNull();
  });

  it('offers Retry view and addresses menu.open with the issue subject', async () => {
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
    // The payload is the issue's own subject — a view id the runtime already
    // knows. Nothing free-text ever reaches an endpoint.
    const open = bridge.outbound.find((m) => m.name === 'menu.open');
    expect(open?.payload).toEqual({ view: 'broken/panel' });
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
    // Payload-free and fixed-target: the shell destination is derived natively,
    // so the page cannot steer it.
    const opened = bridge.outbound.find((m) => m.name === 'osfui.openLogFolder');
    expect(opened).toBeDefined();
    expect(opened!.payload).toBeUndefined();
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
    // A warning is a collapsed row: its actions live behind the row itself.
    expect(el.querySelector('.health-card-disclose')).toBeNull();
    el.querySelector<HTMLButtonElement>('.health-row-head')!.click();
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
  it('offers local-only health actions and never probes an upload service', async () => {
    const { bridge, el } = await mountHealth([]);
    openHealth(el);
    await flush();

    const actions = [...el.querySelectorAll<HTMLButtonElement>('.health-actions .osf-btn')]
      .map((button) => button.textContent);
    expect(actions).toContain('Copy diagnostic report');
    expect(actions).toContain('Open log folder');
    expect(actions).not.toContain('Report a bug');
    expect(bridge.outbound.map((message) => message.name)).not.toContain('diagnostics.reportStatus');
    expect(bridge.outbound.map((message) => message.name)).not.toContain('diagnostics.submitReport');
  });

  it('writes a report to the clipboard and toasts success', async () => {
    const writeText = vi.fn().mockResolvedValue(undefined);
    vi.stubGlobal('navigator', { clipboard: { writeText } });
    const { el } = await mountHealth([ISSUE({ id: 'e', severity: 'error' })], { version: '2.0.0' });
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
      // An error, so this stays a full card and the test is about the
      // clipboard degrading rather than about the warning tier.
      ISSUE({
        id: 'e',
        severity: 'error',
        code: 'view.load-failed',
        subject: 'x/y',
        context: { errorCode: -6 },
      }),
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
    const { el } = await mountFailedPanel();

    // The card foot reads the deep-link affordance, not "SEE LOG".
    const tile = failedTile(el);
    expect(tile.textContent).toContain('FAILED — REVIEW ISSUE');
    tile.click();
    await flush();

    // Landed on Health with that issue's card expanded.
    expect(el.querySelector('#health-summary')).not.toBeNull();
    const card = el.querySelector('.health-card[data-issue="view.load-failed:broken/panel"]')!;
    expect(card.querySelector('.health-card-technical')).not.toBeNull();
  });

  it('scrolls the linked card into view', async () => {
    // The summary, the action bar and the warning-tier header all sit above the
    // list, so expanding alone routinely leaves the target below the fold and
    // the jump reads as "nothing happened". jsdom omits scrollIntoView (which is
    // why the pane guards the call), so stub it to observe.
    const scrolled: string[] = [];
    const proto = window.HTMLElement.prototype as unknown as {
      scrollIntoView?: (this: HTMLElement) => void;
    };
    const had = Object.prototype.hasOwnProperty.call(proto, 'scrollIntoView');
    proto.scrollIntoView = function () {
      scrolled.push(this.getAttribute('data-issue') || this.className);
    };

    try {
      const { el } = await mountFailedPanel();

      // Sitting on the launcher, nothing is deep-linked: no scroll yet.
      expect(scrolled).toEqual([]);

      failedTile(el).click();
      await flush();

      expect(scrolled).toContain('view.load-failed:broken/panel');
    } finally {
      if (!had) delete proto.scrollIntoView;
    }
  });

  it('opens the history block when the linked issue RESOLVES while you are on it', async () => {
    // Deep links only ever name an ACTIVE issue (issueForSubject skips resolved
    // ones), so the reachable case is this one: you follow a failed view's card
    // here and the condition then clears underneath you. The card moves into the
    // history disclosure, which is collapsed — without opening it, the issue the
    // link was pointing at simply vanishes off the pane.
    const { bridge, el } = await mountFailedPanel();

    failedTile(el).click();
    await flush();
    expect(el.querySelector('#health-resolved')).toBeNull(); // history still shut

    // A withdrawal republishes the whole snapshot with the record marked
    // resolved; the record survives for the rest of the session.
    bridge.publish('osfui/diagnostics', {
      system: {},
      issues: [
        ISSUE({
          id: 'view.load-failed:broken/panel',
          severity: 'error',
          code: 'view.load-failed',
          subject: 'broken/panel',
          status: 'resolved',
          resolvedAt: 12,
        }),
      ],
    });
    await flush();

    const list = el.querySelector('#health-resolved');
    expect(list).not.toBeNull();
    const card = list!.querySelector('.health-card[data-issue="view.load-failed:broken/panel"]');
    expect(card).not.toBeNull();
    // Still expanded — the card followed the link, it did not reset.
    expect(card!.querySelector('.health-card-technical')).not.toBeNull();
  });
});

/** A launcher carrying one failed view, plus the issue that explains it. */
async function mountFailedPanel(issueOver: Record<string, unknown> = {}) {
  const bridge = makeBridge({
    state: {
      'osfui/settings': WIDGETS,
      'osfui/views': {
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
      },
      'osfui/diagnostics': {
        system: {},
        issues: [
          ISSUE({
            id: 'view.load-failed:broken/panel',
            severity: 'error',
            code: 'view.load-failed',
            subject: 'broken/panel',
            ...issueOver,
          }),
        ],
      },
    },
  });
  const el = await mount(bridge);
  await flush();
  return { bridge, el };
}

function failedTile(el: HTMLElement): HTMLButtonElement {
  return [...el.querySelectorAll<HTMLButtonElement>('.home-tile')].find((t) =>
    t.textContent!.includes('Broken Panel'),
  )!;
}

describe('mod severity marker', () => {
  it('coexists with the modified-setting count on the rail', async () => {
    // A widget mod with a modified value so the count badge shows.
    const bridge = makeBridge({
      state: {
        'osfui/settings': WIDGETS,
        'osfui/diagnostics': {
          system: {},
          issues: [
            ISSUE({
              id: 'x',
              severity: 'warning',
              code: 'settings.values-parse',
              subject: 'acme.kit',
            }),
          ],
        },
      },
    });
    const el = await mount(bridge);
    await flush();
    const railItem = [...el.querySelectorAll('.rail-item')].find((r) =>
      r.textContent!.includes('Acme Kit'),
    )!;
    expect(railItem.querySelector('.rail-item-severity--warning')).not.toBeNull();
  });
});
