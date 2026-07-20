// @vitest-environment jsdom
//
// dom-contracts.test.tsx — the padnav coupling, pinned.
//
// src/legacy/padnav.js ships VERBATIM and navigates by reading the DOM. It is
// not imported, not bundled and not type-checked, so nothing else in this repo
// can tell you when a component stops producing the shape it queries — the
// symptom is a controller that silently cannot reach a control in game.
//
// Every assertion below quotes the padnav line that consumes it. If one of
// these fails, the fix is in the component, never in padnav.

import { describe, it, expect, afterEach } from 'vitest';
import { render } from 'preact';
import { act } from 'preact/test-utils';
import { Row } from '@ui/Row';
import { Overlay } from '@ui/Overlay';
import { App } from '@views/osfui/keybinds/App';
import { nullBridge, type Bridge } from '@lib/bridge';
import type { SettingsDataPayload } from '@sdk';

// ---------------------------------------------------------------------------
// harness
// ---------------------------------------------------------------------------

type Listener = (payload: unknown) => void;

interface FakeBridge extends Bridge {
  /** Deliver a native->web push to whatever the view subscribed. */
  emit(type: string, payload: unknown): void;
  sent: Array<{ command: string; fields?: Record<string, unknown> }>;
  requests: Array<{ command: string; fields?: Record<string, unknown> }>;
  /** Settle the Nth pending request. */
  settle(index: number, value: unknown): void;
  reject(index: number, err: unknown): void;
}

function makeBridge(): FakeBridge {
  const listeners = new Map<string, Set<Listener>>();
  const pending: Array<{ resolve: (v: unknown) => void; reject: (e: unknown) => void }> = [];
  const bridge: FakeBridge = {
    ...nullBridge,
    available: () => true,
    sent: [],
    requests: [],
    send(command, fields) {
      bridge.sent.push(fields === undefined ? { command } : { command, fields });
      return true;
    },
    request(command: string, fields?: Record<string, unknown>) {
      bridge.requests.push(fields === undefined ? { command } : { command, fields });
      return new Promise((resolve, reject) => {
        pending.push({ resolve: resolve as (v: unknown) => void, reject });
      }) as never;
    },
    on(type: string, fn: unknown) {
      let set = listeners.get(type);
      if (!set) {
        set = new Set();
        listeners.set(type, set);
      }
      set.add(fn as Listener);
      return () => {
        const s = listeners.get(type);
        if (s) s.delete(fn as Listener);
      };
    },
    emit(type, payload) {
      const set = listeners.get(type);
      if (set) for (const fn of [...set]) fn(payload);
    },
    settle(index, value) {
      const p = pending[index];
      if (p) p.resolve(value);
    },
    reject(index, err) {
      const p = pending[index];
      if (p) p.reject(err);
    },
  } as FakeBridge;
  return bridge;
}

/**
 * Let Preact settle.
 *
 * `act` is what actually matters here: Preact schedules useEffect callbacks
 * with requestAnimationFrame, so without it the bridge subscriptions and the
 * document-level keydown listener are never installed and every push is
 * silently dropped. The inner timeout drains promise callbacks (the bridge
 * request chains) too.
 */
const flush = async () => {
  await act(async () => {
    await new Promise((r) => setTimeout(r, 0));
  });
};

const DATA: SettingsDataPayload = {
  mods: [
    {
      id: 'osfui',
      title: 'OSF UI',
      values: { toggleKey: 'F10' },
      schema: {
        groups: [
          { settings: [{ key: 'toggleKey', label: 'Open / close key', type: 'key' }] },
        ],
      },
    },
  ],
  vanillaKeys: [{ event: 'QuickSave', title: 'Starfield (Quicksave)', name: 'F5' }],
} as unknown as SettingsDataPayload;

let host: HTMLElement | null = null;

async function mount(bridge: Bridge) {
  host = document.createElement('div');
  document.body.appendChild(host);
  // The initial render MUST be inside `act`, not merely followed by it:
  // Preact queues useEffect callbacks through `afterPaint`, and a render
  // performed outside an act scope leaves that queue unflushed, so the view
  // mounts with none of its bridge subscriptions installed.
  await act(async () => {
    render(<App bridge={bridge} />, host as HTMLElement);
  });
  await flush();
  return host;
}

afterEach(() => {
  if (host) {
    render(null, host);
    host.remove();
    host = null;
  }
  document.body.innerHTML = '';
});

// ---------------------------------------------------------------------------

describe('padnav DOM contracts', () => {
  it('Row emits class="row" — the navigation band padnav measures against', () => {
    // padnav.js:99-102 `bandOf`: `el.closest(".row")` decides whether two
    // controls count as one navigation line. No `.row`, no band.
    const el = document.createElement('div');
    render(
      <Row class="" dataLabel="" dataKey="">
        <button type="button">x</button>
      </Row>,
      el,
    );
    const row = el.querySelector('div');
    expect(row).not.toBeNull();
    expect(row!.classList.contains('row')).toBe(true);
    // Extra classes must APPEND, never replace the contract class.
    render(
      <Row class="row-danger" dataLabel="open / close key" dataKey="toggleKey">
        <button type="button">x</button>
      </Row>,
      el,
    );
    const decorated = el.querySelector('div')!;
    expect(decorated.className).toBe('row row-danger');
    expect(decorated.getAttribute('data-label')).toBe('open / close key');
    expect(decorated.getAttribute('data-key')).toBe('toggleKey');
    render(null, el);
  });

  it('Overlay emits data-nav-modal="1" — the focus trap', () => {
    // padnav.js:76 `navRoot()` and :211 both query `[data-nav-modal]` by
    // ATTRIBUTE PRESENCE, so any value traps; "1" matches the shipped markup
    // (settings/main.legacy.js:1505).
    const el = document.createElement('div');
    render(
      <Overlay class="session-overlay">
        <button type="button">revert</button>
      </Overlay>,
      el,
    );
    const overlay = el.querySelector('.session-overlay');
    expect(overlay).not.toBeNull();
    expect(overlay!.getAttribute('data-nav-modal')).toBe('1');
    // The selector padnav actually uses must find it.
    expect(el.querySelector('[data-nav-modal]')).toBe(overlay);
    render(null, el);
  });

  it('bind-list rows carry tabIndex=0 so Enter/A can reach them', async () => {
    // padnav.js:85-86 enumerates `button, input, select, textarea, a[href],
    // [tabindex]` and skips anything with `tabIndex < 0`. A click-to-select div
    // is invisible to it without an explicit tabindex.
    const bridge = makeBridge();
    const el = await mount(bridge);
    bridge.emit('settings.data', DATA);
    await flush();

    const rows = el.querySelectorAll<HTMLElement>('#bindlist .kb-holder--list');
    expect(rows.length).toBeGreaterThan(0);
    for (const row of rows) {
      expect(row.tabIndex).toBe(0);
      expect(row.getAttribute('tabindex')).toBe('0');
    }

    // Detail-panel rows are NOT focusable — they have no row-level action, so
    // making them targets would add dead stops to the navigation path.
    // F5 specifically — the first LIVE cell (F1) holds nothing, so it would
    // render the empty-state hint instead of any rows.
    [...el.querySelectorAll<HTMLButtonElement>('#keyboard button')]
      .find((c) => c.querySelector('.kb-key-label')!.textContent === 'F5')!
      .click();
    await flush();
    const detailRows = el.querySelectorAll<HTMLElement>('#detail .kb-holder');
    expect(detailRows.length).toBeGreaterThan(0);
    for (const row of detailRows) {
      expect(row.hasAttribute('tabindex')).toBe(false);
      expect(row.classList.contains('kb-holder--list')).toBe(false);
    }
  });

  it('dead keyboard cells render disabled so navigation skips them', async () => {
    // padnav.js:87 `if (el.disabled || el.tabIndex < 0) continue;`. Without
    // `disabled` the arrow keys would stop on a cell that can never be bound.
    //
    // Esc is the ONLY such cell now: the punctuation keys became bindable once
    // native learned their names, so this asserts the reserved-Esc contract
    // rather than a count.
    const bridge = makeBridge();
    const el = await mount(bridge);
    bridge.emit('settings.data', DATA);
    await flush();

    const dead = el.querySelectorAll<HTMLButtonElement>('#keyboard button.is-dead');
    expect(dead.length).toBe(1);
    for (const cell of dead) {
      expect(cell.disabled).toBe(true);
    }
    // The punctuation keys are live cells now — bindable, and not skipped by
    // padnav. Guards the regression of them silently going dead again.
    const punctuation = [...el.querySelectorAll<HTMLButtonElement>('#keyboard button')]
      .filter((c) => ['-', '=', '[', ']', '\\', ';', "'", ',', '.', '/'].includes(c.textContent!));
    expect(punctuation.length).toBe(10);
    for (const cell of punctuation) {
      expect(cell.classList.contains('is-dead')).toBe(false);
      expect(cell.disabled).toBe(false);
    }
    // Esc gets its own reason: it IS resolvable natively, but the capture flow
    // reads a press of it as "cancel".
    const esc = [...dead].find((c) => c.textContent === 'Esc');
    expect(esc).toBeDefined();
    expect(esc!.title).toBe('Reserved (cancels rebinds)');

    // Live cells are the inverse: enabled, and never marked dead.
    const live = el.querySelectorAll<HTMLButtonElement>('#keyboard button:not(.is-dead)');
    expect(live.length).toBeGreaterThan(0);
    for (const cell of live) expect(cell.disabled).toBe(false);
  });

  it('an armed capture puts class="listening" in the document', async () => {
    // padnav.js:207 `if (document.querySelector(".listening")) return;` —
    // ALL navigation suspends while a rebind is armed, because the next key
    // press belongs to the capture.
    const bridge = makeBridge();
    const el = await mount(bridge);
    bridge.emit('settings.data', DATA);
    await flush();

    expect(document.querySelector('.listening')).toBeNull();

    const rebind = el.querySelector<HTMLButtonElement>('#bindlist .osf-key');
    expect(rebind).not.toBeNull();
    rebind!.click();
    await flush();

    const listening = el.querySelector('.listening');
    expect(listening).not.toBeNull();
    // It is the BUTTON that listens, and it keeps its kit classes.
    expect(listening!.tagName).toBe('BUTTON');
    expect(listening!.className).toBe('osf-btn osf-btn--sm osf-key listening');

    // Exactly one, even though the same binding may also be on screen in the
    // detail panel: legacy arms the clicked BUTTON, not the binding.
    expect(document.querySelectorAll('.listening').length).toBe(1);

    // ...and the capture really did go out as one open-ended request.
    expect(bridge.requests[0]).toEqual({
      command: 'settings.captureKey',
      fields: { mod: 'osfui', key: 'toggleKey' },
    });
  });

  it('only the clicked instance listens when a binding is on screen twice', async () => {
    const bridge = makeBridge();
    const el = await mount(bridge);
    bridge.emit('settings.data', DATA);
    await flush();

    // Select F10 so the mod binding renders in the detail panel AND the list.
    const f10 = [...el.querySelectorAll<HTMLButtonElement>('#keyboard button')].find(
      (c) => c.textContent!.startsWith('F10'),
    );
    f10!.click();
    await flush();
    expect(el.querySelectorAll('.osf-key').length).toBe(2);

    el.querySelector<HTMLButtonElement>('#detail .osf-key')!.click();
    await flush();

    expect(document.querySelectorAll('.listening').length).toBe(1);
    expect(el.querySelector('#detail .listening')).not.toBeNull();
    expect(el.querySelector('#bindlist .listening')).toBeNull();
  });
});
