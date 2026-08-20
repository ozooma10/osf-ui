// @vitest-environment jsdom

import { describe, it, expect, afterEach } from 'vitest';
import { render } from 'preact';
import { act } from 'preact/test-utils';
import { App } from '@views/osfui/keybinds/App';
import { nullBridge, type Bridge } from '@lib/bridge';
import type { KeybindingsData, SettingsData } from '@sdk';

type Handler = (value: unknown) => void;

interface OutboundMessage {
  name: string;
  payload?: Record<string, unknown>;
}

interface FakeBridge extends Bridge {
  /** Fire a one-shot EVENT. Never replayed. */
  deliver(event: string, payload: unknown): void;
  /** Publish a STATE key: replayed to every later subscriber. */
  publish(key: string, value: unknown): void;
  sent: OutboundMessage[];
  requests: OutboundMessage[];
  settle(index: number, value: unknown): void;
  reject(index: number, err: unknown): void;
}

function makeBridge(state: Record<string, unknown> = {}): FakeBridge {
  const eventListeners = new Map<string, Set<Handler>>();
  const stateListeners = new Map<string, Set<Handler>>();
  const values = new Map<string, unknown>(Object.entries(state));
  const pending: Array<{ resolve: (v: unknown) => void; reject: (e: unknown) => void }> = [];

  const subscribe = (map: Map<string, Set<Handler>>, key: string, fn: Handler) => {
    let set = map.get(key);
    if (!set) {
      set = new Set();
      map.set(key, set);
    }
    set.add(fn);
    return () => {
      map.get(key)?.delete(fn);
    };
  };

  const bridge: FakeBridge = {
    ...nullBridge,
    available: () => true,
    sent: [],
    requests: [],
    send(name: string, payload?: Record<string, unknown>) {
      bridge.sent.push(payload === undefined ? { name } : { name, payload });
      return true;
    },
    request(name: string, payload?: Record<string, unknown>) {
      bridge.requests.push(payload === undefined ? { name } : { name, payload });
      return new Promise((resolve, reject) => {
        pending.push({ resolve: resolve as (v: unknown) => void, reject });
      }) as never;
    },
    on(event: string, fn: unknown) {
      return subscribe(eventListeners, event, fn as Handler);
    },
    onAny(event: string, fn: unknown) {
      return subscribe(eventListeners, event, fn as Handler);
    },
    state(key: string, fn: unknown) {
      const off = subscribe(stateListeners, key, fn as Handler);
      // Subscribing IS the read.
      if (values.has(key)) (fn as Handler)(values.get(key));
      return off;
    },
    deliver(event, payload) {
      const set = eventListeners.get(event);
      if (set) for (const fn of [...set]) fn(payload);
    },
    publish(key, value) {
      values.set(key, value);
      const set = stateListeners.get(key);
      if (set) for (const fn of [...set]) fn(value);
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

const flush = async () => {
  await act(async () => {
    await new Promise((r) => setTimeout(r, 0));
  });
};

const DATA: SettingsData = {
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
} as unknown as SettingsData;

const LIVE_KEYBINDINGS: KeybindingsData = {
  available: true,
  revision: 1,
  gameVersion: 'test',
  actions: [
    {
      event: 'QuickSave', label: 'Quicksave', category: 'MainGameplay',
      context: { id: 0, name: 'MainGameplay', order: 0 }, classification: 'core',
      modes: { definite: ['ship'], possible: [] }, sortIndex: 0, required: false,
      bindings: [{ slot: 'main', key: 'F5', chord: ['F5'], unbound: false }],
    },
    {
      event: 'Activate', label: 'Interact', category: 'OnFoot',
      context: { id: 1, name: 'OnFoot', order: 1 }, classification: 'core',
      modes: { definite: ['onFoot'], possible: [] }, sortIndex: 1, required: false,
      bindings: [{ slot: 'main', key: 'E', chord: ['E'], unbound: false }],
    },
  ],
};

/** A bridge whose document already received the settings replay. */
const seeded = () => makeBridge({
  'osfui/settings': DATA,
  'osfui/keybindings': LIVE_KEYBINDINGS,
});
const seededWithLiveKeys = seeded;

let host: HTMLElement | null = null;

async function mount(bridge: Bridge) {
  host = document.createElement('div');
  document.body.appendChild(host);
  await act(async () => {
    render(<App bridge={bridge} />, host as HTMLElement);
  });
  await flush();
  return host;
}

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

async function chooseBindingFilter(el: HTMLElement, label: string) {
  el.querySelector<HTMLButtonElement>('#binding-filter')!.click();
  await flush();
  const option = [...document.querySelectorAll<HTMLElement>('[role="option"]')]
    .find((candidate) => candidate.textContent === label);
  if (!option) throw new Error(`no binding filter option labelled ${label}`);
  option.click();
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

describe('Keybindings — selection', () => {
  it('selectKey TOGGLES: clicking the selected key deselects it', async () => {
    // Toggling is the only way to clear the panel — there is no close affordance.
    const el = await mount(seeded());

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

    cell(el, 'F10').click();
    await flush();
    cell(el, 'F5').click();
    await flush();
    expect(title()).toContain('F5');
    expect(cell(el, 'F10').classList.contains('is-selected')).toBe(false);
    expect(cell(el, 'F5').classList.contains('is-selected')).toBe(true);
  });
});

describe('Keybindings — search scope', () => {
  it('repaints the board and the list but NOT the detail panel', async () => {
    const el = await mount(seeded());

    // Select F5 — a mod binding plus Starfield's Quicksave binding, i.e. a conflict.
    cell(el, 'F5').click();
    await flush();

    const detailBefore = el.querySelector('#detail')!.innerHTML;
    const detailTitleBefore = el.querySelector('#detail-title')!.innerHTML;
    const listBefore = el.querySelector('#bindlist')!.innerHTML;

    // A query that matches nothing in the detail panel's key.
    await typeSearch(el, 'interact');

    // The list narrowed...
    expect(el.querySelector('#bindlist')!.innerHTML).not.toBe(listBefore);
    expect(el.querySelectorAll('#bindlist .kb-holder--list').length).toBe(1);
    expect(el.querySelector('#list-title')!.textContent).toBe('All bindings (1)');

    // ...the board dimmed the non-matching keys...
    expect(cell(el, 'F10').classList.contains('is-dim')).toBe(true);
    expect(cell(el, 'E').classList.contains('is-dim')).toBe(false);
    expect(cell(el, 'E').classList.contains('is-prioritized')).toBe(true);

    // ...and the detail panel is byte-identical.
    expect(el.querySelector('#detail')!.innerHTML).toBe(detailBefore);
    expect(el.querySelector('#detail-title')!.innerHTML).toBe(detailTitleBefore);
    expect(el.querySelectorAll('#detail .kb-holder').length).toBe(2);
  });

  it('dims a key only when neither its holders nor its own name match', async () => {
    const el = await mount(seeded());

    await typeSearch(el, 'f11'); // nothing is bound to F11
    expect(cell(el, 'F11').classList.contains('is-dim')).toBe(false);
    expect(cell(el, 'F10').classList.contains('is-dim')).toBe(true);
  });
});

describe('Keybindings — list priority on the keyboard map', () => {
  it('prioritizes occupied keys represented by the active list filter', async () => {
    const el = await mount(seededWithLiveKeys());

    await chooseBindingFilter(el, 'MainGameplay');

    expect(el.querySelector('#list-title')!.textContent).toBe('All bindings (1)');
    expect(cell(el, 'F5').classList.contains('is-prioritized')).toBe(true);
    expect(cell(el, 'F10').classList.contains('is-dim')).toBe(true);
    expect(cell(el, 'E').classList.contains('is-dim')).toBe(true);
    expect(cell(el, 'F11').classList.contains('is-dim')).toBe(true);
    expect(el.querySelector('.legend-prioritized')?.parentElement?.textContent).toContain('In list');
  });

  it('lists selected-key holders from the selected Layer first without hiding the rest', async () => {
    const el = await mount(seededWithLiveKeys());

    cell(el, 'F5').click();
    await flush();
    const titles = () => [...el.querySelectorAll<HTMLElement>('#detail .kb-holder-title')]
      .map((row) => row.textContent);
    expect(titles()[0]).toContain('Open panel');
    expect(titles()[1]).toContain('Quicksave');

    await chooseBindingFilter(el, 'MainGameplay');

    expect(titles()).toHaveLength(2);
    expect(titles()[0]).toContain('Quicksave');
    expect(titles()[1]).toContain('Open panel');
  });
});

describe('Keybindings — list row activation', () => {
  it('IGNORES a row click that landed inside a button', async () => {
    const bridge = seeded();
    const el = await mount(bridge);

    expect(el.querySelector('#detail-title')!.textContent).toBe('Select a key');

    const rebind = el.querySelector<HTMLButtonElement>('#bindlist .osf-key')!;
    rebind.click();
    await flush();

    // The capture armed, and the selection did not move.
    expect(document.querySelectorAll('.listening').length).toBe(1);
    expect(el.querySelector('#detail-title')!.textContent).toBe('Select a key');
    expect(bridge.requests[0]!.name).toBe('settings.captureKey');

    // Control case: a click elsewhere in the same row does select.
    const row = rebind.closest('.kb-holder--list') as HTMLElement;
    const chip = row.querySelector('.kb-chip') as HTMLElement;
    chip.dispatchEvent(new MouseEvent('click', { bubbles: true }));
    await flush();
    expect(el.querySelector('#detail-title')!.textContent).toContain(chip.textContent!);
  });
});

describe('Keybindings — capture settles in two steps', () => {
  it('arms with a request and commits from the settings.captured EVENT', async () => {
    const bridge = seeded();
    const el = await mount(bridge);

    // The first bind-list row is demo/panelKey (F5).
    el.querySelector<HTMLButtonElement>('#bindlist .osf-key')!.click();
    await flush();
    const armed = bridge.requests.findIndex((r) => r.name === 'settings.captureKey');
    expect(bridge.requests[armed]).toEqual({
      name: 'settings.captureKey',
      payload: { mod: 'demo', key: 'panelKey' },
    });

    bridge.settle(armed, { armed: true, mod: 'demo', key: 'panelKey' });
    await flush();
    // Armed is not captured: the button keeps listening.
    expect(document.querySelectorAll('.listening').length).toBe(1);

    bridge.deliver('settings.captured', {
      mod: 'demo',
      key: 'panelKey',
      name: 'F9',
      cancelled: false,
    });
    await flush();

    expect(document.querySelector('.listening')).toBeNull();
    // The commit is written back through the normal write endpoint.
    expect(bridge.requests.find((r) => r.name === 'settings.set')).toEqual({
      name: 'settings.set',
      payload: { mod: 'demo', key: 'panelKey', value: 'F9' },
    });
  });
});

describe('Keybindings — goBack', () => {
  it('opens Mod Settings, and falls back to a bare close when that rejects', async () => {
    const bridge = seeded();
    const el = await mount(bridge);
    expect(bridge.sent.find((s) => s.name === 'osfui.handleBack')).toEqual({
      name: 'osfui.handleBack',
      payload: { handle: true, view: 'osfui/settings' },
    });

    el.querySelector<HTMLButtonElement>('#back')!.click();
    await flush();

    const open = bridge.requests.find((r) => r.name === 'menu.open');
    expect(open).toEqual({ name: 'menu.open', payload: { view: 'osfui/settings' } });
    // Nothing closed yet — Mod Settings is expected to take over.
    expect(bridge.sent.some((s) => s.name === 'close')).toBe(false);

    const err = Object.assign(new Error('unknown view'), { code: 'unknown-view' });
    bridge.reject(bridge.requests.indexOf(open!), err);
    await flush();

    expect(bridge.sent.some((s) => s.name === 'close')).toBe(true);
  });

  it('Escape reaches goBack, and is SWALLOWED while a capture is armed', async () => {
    const bridge = seeded();
    const el = await mount(bridge);

    const escape = () =>
      document.dispatchEvent(
        new KeyboardEvent('keydown', { key: 'Escape', keyCode: 27, bubbles: true }),
      );

    escape();
    await flush();
    expect(bridge.requests.filter((r) => r.name === 'menu.open').length).toBe(1);

    // Arm a rebind, then press Escape: it belongs to the capture, not to us.
    el.querySelector<HTMLButtonElement>('#bindlist .osf-key')!.click();
    await flush();
    escape();
    await flush();
    expect(bridge.requests.filter((r) => r.name === 'menu.open').length).toBe(1);
  });
});
