// @vitest-environment jsdom
//
// keybinds.navigation.test.tsx — the four behaviours of the keybinds view that
// a well-meaning refactor would "fix" into something else.
//
// Each of these is deliberate shipped behaviour, and each looks like a bug from
// the outside:
//   * selecting the selected key DESELECTS it;
//   * searching re-scopes the board and the list but NOT the detail panel;
//   * a click inside a row's button is not a click on the row;
//   * goBack navigates to the hub and only closes the overlay if that fails.

import { describe, it, expect, afterEach } from 'vitest';
import { render } from 'preact';
import { act } from 'preact/test-utils';
import { App } from '@views/osfui/keybinds/App';
import { nullBridge, type Bridge } from '@lib/bridge';
import type { SettingsDataPayload } from '@sdk';

// ---------------------------------------------------------------------------
// harness
// ---------------------------------------------------------------------------

type Listener = (payload: unknown) => void;

interface FakeBridge extends Bridge {
  emit(type: string, payload: unknown): void;
  sent: Array<{ command: string; fields?: Record<string, unknown> }>;
  requests: Array<{ command: string; fields?: Record<string, unknown> }>;
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

/**
 * Two mods and two vanilla rows. `osfui.toggleKey` is F10; `demo.panelKey` is
 * F5, which COLLIDES with the vanilla Quicksave — so the fixture exercises the
 * conflict paths as well as the plain ones.
 */
const DATA: SettingsDataPayload = {
  mods: [
    {
      id: 'osfui',
      title: 'OSF UI',
      values: { toggleKey: 'F10' },
      schema: {
        groups: [{ settings: [{ key: 'toggleKey', label: 'Open / close key', type: 'key' }] }],
      },
    },
    {
      id: 'demo',
      title: 'Demo Mod',
      values: { panelKey: 'F5' },
      schema: {
        groups: [{ settings: [{ key: 'panelKey', label: 'Open panel', type: 'key' }] }],
      },
    },
  ],
  vanillaKeys: [
    { event: 'QuickSave', title: 'Starfield (Quicksave)', name: 'F5' },
    { event: 'Activate', title: 'Starfield (Interact)', name: 'E' },
  ],
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

/** The board cell whose printed label is `label`. */
function cell(el: HTMLElement, label: string): HTMLButtonElement {
  const found = [...el.querySelectorAll<HTMLButtonElement>('#keyboard button')].find(
    (c) => c.querySelector('.kb-key-label')!.textContent === label,
  );
  if (!found) throw new Error(`no board cell labelled ${label}`);
  return found;
}

async function typeSearch(el: HTMLElement, value: string) {
  const input = el.querySelector<HTMLInputElement>('#search')!;
  input.value = value;
  input.dispatchEvent(new Event('input', { bubbles: true }));
  await flush();
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

describe('keybinds — selection', () => {
  it('selectKey TOGGLES: clicking the selected key deselects it', async () => {
    // main.legacy.js:281 `selectedKey = name === selectedKey ? "" : name;`
    // This is the only way to clear the panel — there is no close affordance.
    const bridge = makeBridge();
    const el = await mount(bridge);
    bridge.emit('settings.data', DATA);
    await flush();

    const title = () => el.querySelector('#detail-title')!.textContent;
    expect(title()).toBe('Select a key');

    cell(el, 'F10').click();
    await flush();
    expect(title()).toContain('F10');
    expect(title()).toContain('1 binding');
    expect(cell(el, 'F10').classList.contains('is-selected')).toBe(true);

    // Same key again -> back to nothing selected.
    cell(el, 'F10').click();
    await flush();
    expect(title()).toBe('Select a key');
    expect(cell(el, 'F10').classList.contains('is-selected')).toBe(false);

    // A DIFFERENT key selects rather than toggling, so the toggle really is
    // an equality test and not a "click clears" rule.
    cell(el, 'F10').click();
    await flush();
    cell(el, 'F5').click();
    await flush();
    expect(title()).toContain('F5');
    expect(cell(el, 'F10').classList.contains('is-selected')).toBe(false);
    expect(cell(el, 'F5').classList.contains('is-selected')).toBe(true);
  });
});

describe('keybinds — search scope', () => {
  it('repaints the board and the list but NOT the detail panel', async () => {
    // main.legacy.js:518 — the input handler calls paintKeyboard() and
    // renderList() and nothing else. Re-scoping the detail panel on every
    // keystroke would make the inspected key vanish while you type its name.
    const bridge = makeBridge();
    const el = await mount(bridge);
    bridge.emit('settings.data', DATA);
    await flush();

    // Select F5 — a mod binding AND the vanilla Quicksave, i.e. a conflict.
    cell(el, 'F5').click();
    await flush();

    const detailBefore = el.querySelector('#detail')!.innerHTML;
    const detailTitleBefore = el.querySelector('#detail-title')!.innerHTML;
    const listBefore = el.querySelector('#bindlist')!.innerHTML;

    // A query that matches NOTHING in the detail panel's key.
    await typeSearch(el, 'interact');

    // The list narrowed...
    expect(el.querySelector('#bindlist')!.innerHTML).not.toBe(listBefore);
    expect(el.querySelectorAll('#bindlist .kb-holder--list').length).toBe(1);
    expect(el.querySelector('#list-title')!.textContent).toBe('All bindings (1)');

    // ...the board dimmed the non-matching keys...
    expect(cell(el, 'F10').classList.contains('is-dim')).toBe(true);
    expect(cell(el, 'E').classList.contains('is-dim')).toBe(false);

    // ...and the detail panel is byte-identical.
    expect(el.querySelector('#detail')!.innerHTML).toBe(detailBefore);
    expect(el.querySelector('#detail-title')!.innerHTML).toBe(detailTitleBefore);
    expect(el.querySelectorAll('#detail .kb-holder').length).toBe(2);
  });

  it('dims a key only when neither its holders nor its own name match', async () => {
    // main.legacy.js:256 — the second clause is why searching "f10" keeps F10
    // lit even for a key nothing is bound to.
    const bridge = makeBridge();
    const el = await mount(bridge);
    bridge.emit('settings.data', DATA);
    await flush();

    await typeSearch(el, 'f11'); // nothing is bound to F11
    expect(cell(el, 'F11').classList.contains('is-dim')).toBe(false);
    expect(cell(el, 'F10').classList.contains('is-dim')).toBe(true);
  });
});

describe('keybinds — list row activation', () => {
  it('IGNORES a row click that landed inside a button', async () => {
    // main.legacy.js:365 `if (e.target.closest("button")) return;` — a Rebind
    // click must stay a rebind. Without it, arming a capture would also change
    // the selection out from under the user.
    const bridge = makeBridge();
    const el = await mount(bridge);
    bridge.emit('settings.data', DATA);
    await flush();

    expect(el.querySelector('#detail-title')!.textContent).toBe('Select a key');

    const rebind = el.querySelector<HTMLButtonElement>('#bindlist .osf-key')!;
    rebind.click();
    await flush();

    // The capture armed, and the selection did NOT move.
    expect(document.querySelectorAll('.listening').length).toBe(1);
    expect(el.querySelector('#detail-title')!.textContent).toBe('Select a key');
    expect(bridge.requests[0]!.command).toBe('settings.captureKey');

    // The control case: a click elsewhere in the same row DOES select.
    const row = rebind.closest('.kb-holder--list') as HTMLElement;
    const chip = row.querySelector('.kb-chip') as HTMLElement;
    chip.dispatchEvent(new MouseEvent('click', { bubbles: true }));
    await flush();
    expect(el.querySelector('#detail-title')!.textContent).toContain(chip.textContent!);
  });
});

describe('keybinds — goBack', () => {
  it('opens the hub, and falls back to a bare close when that rejects', async () => {
    // main.legacy.js:525-529. Single-menu policy means opening the hub REPLACES
    // this menu, so the happy path needs no close at all. The fallback exists
    // so an unregistered hub view cannot strand the user in a menu they have no
    // way to leave.
    const bridge = makeBridge();
    const el = await mount(bridge);
    bridge.emit('settings.data', DATA);
    await flush();

    el.querySelector<HTMLButtonElement>('#back')!.click();
    await flush();

    const open = bridge.requests.find((r) => r.command === 'menu.open');
    expect(open).toEqual({ command: 'menu.open', fields: { view: 'osfui/settings' } });
    // Nothing closed yet — the hub is expected to take over.
    expect(bridge.sent.some((s) => s.command === 'close')).toBe(false);

    const err = Object.assign(new Error('unknown view'), { code: 'unknown-view' });
    bridge.reject(bridge.requests.indexOf(open!), err);
    await flush();

    expect(bridge.sent.some((s) => s.command === 'close')).toBe(true);
  });

  it('Escape reaches goBack, and is SWALLOWED while a capture is armed', async () => {
    const bridge = makeBridge();
    const el = await mount(bridge);
    bridge.emit('settings.data', DATA);
    await flush();

    const escape = () =>
      document.dispatchEvent(
        new KeyboardEvent('keydown', { key: 'Escape', keyCode: 27, bubbles: true }),
      );

    escape();
    await flush();
    expect(bridge.requests.filter((r) => r.command === 'menu.open').length).toBe(1);

    // Arm a rebind, then press Escape: it belongs to the capture, not to us.
    el.querySelector<HTMLButtonElement>('#bindlist .osf-key')!.click();
    await flush();
    escape();
    await flush();
    expect(bridge.requests.filter((r) => r.command === 'menu.open').length).toBe(1);
  });
});
