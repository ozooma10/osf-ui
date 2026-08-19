// @vitest-environment jsdom

import { describe, it, expect, afterEach } from 'vitest';
import { render } from 'preact';
import { act } from 'preact/test-utils';
import { Row } from '@ui/Row';
import { Overlay } from '@ui/Overlay';
import { KeyField } from '@ui/KeyField';
import { ActionButton } from '@ui/ActionButton';
import { App } from '@views/osfui/keybinds/App';
import { nullBridge, type Bridge } from '@lib/bridge';
import type { KeybindingsData, RuntimeInfo, SettingsData } from '@sdk';

// Harness

type Handler = (value: unknown) => void;

interface OutboundMessage {
  name: string;
  payload?: Record<string, unknown>;
}

interface FakeBridge extends Bridge {
  /** Fire a one-shot EVENT at whatever subscribed through on(). */
  deliver(event: string, payload: unknown): void;
  /** Publish a STATE key: replayed to every later subscriber. */
  publish(key: string, value: unknown): void;
  sent: OutboundMessage[];
  requests: OutboundMessage[];
  /** Settle the Nth pending request. */
  settle(index: number, value: unknown): void;
  reject(index: number, err: unknown): void;
}

const RUNTIME: RuntimeInfo = {
  game: 'Starfield',
  plugin: 'OSF UI',
  version: '2.0.0',
  bridgeVersion: '2.0',
  view: 'osfui/keybinds',
  mod: 'osfui',
};

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
    ready: () => Promise.resolve(RUNTIME),
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
      if (values.has(key)) (fn as Handler)(values.get(key));
      return off;
    },
    peek(key: string) {
      return values.get(key) as never;
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
        groups: [
          { settings: [{ key: 'toggleKey', label: 'Open / close key', type: 'key' }] },
        ],
      },
    },
  ],
} as unknown as SettingsData;

const LIVE_KEYBINDINGS: KeybindingsData = {
  available: true,
  revision: 1,
  gameVersion: 'test',
  actions: [{
    event: 'QuickSave', label: 'Quicksave', category: 'MainGameplay',
    context: { id: 0, name: 'MainGameplay', order: 0 }, classification: 'core',
    modes: { definite: ['onFoot'], possible: [] }, sortIndex: 0, required: false,
    bindings: [{ slot: 'main', key: 'F5', chord: ['F5'], unbound: false }],
  }],
};

/** A bridge whose document already received both retained-state replays. */
const seeded = () => makeBridge({
  'osfui/settings': DATA,
  'osfui/keybindings': LIVE_KEYBINDINGS,
});

const GERMAN_DATA: SettingsData = {
  ...DATA,
  keyboard: {
    layout: 'de-DE',
    labels: { Semicolon: 'Ö', Grave: '^', Y: 'Z', Z: 'Y', IntlBackslash: '<', Minus: 'ß' },
  },
} as unknown as SettingsData;
const seededGerman = () => makeBridge({
  'osfui/settings': GERMAN_DATA,
  'osfui/keybindings': LIVE_KEYBINDINGS,
});

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

afterEach(() => {
  if (host) {
    render(null, host);
    host.remove();
    host = null;
  }
  document.body.innerHTML = '';
});

describe('padnav DOM contracts', () => {
  it('Row emits class="row" — the navigation band padnav measures against', () => {
    const el = document.createElement('div');
    render(
      <Row class="" dataKey="">
        <button type="button">x</button>
      </Row>,
      el,
    );
    const row = el.querySelector('div');
    expect(row).not.toBeNull();
    expect(row!.classList.contains('row')).toBe(true);
    // Extra classes must append, not replace the contract class.
    render(
      <Row class="row-danger" dataKey="toggleKey">
        <button type="button">x</button>
      </Row>,
      el,
    );
    const decorated = el.querySelector('div')!;
    expect(decorated.className).toBe('row row-danger');
    expect(decorated.getAttribute('data-key')).toBe('toggleKey');
    render(null, el);
  });

  it('KeyField appends .listening LAST — the class padnav suspends navigation on', () => {
    const el = document.createElement('div');
    const props = {
      id: 'k',
      value: 'F10',
      allowUnbound: false,
      disabled: false,
      onRebind: () => {},
      onUnbind: () => {},
      listeningLabel: 'Press a key…',
      unbindTitle: 'Unbind',
      unbindLabel: 'Unbind setting',
    };
    render(<KeyField {...props} listening={false} />, el);
    expect(el.querySelector('button')!.className).toBe('osf-btn osf-btn--sm osf-key');
    render(<KeyField {...props} listening />, el);
    expect(el.querySelector('button')!.className).toBe('osf-btn osf-btn--sm osf-key listening');
    // And the selector padnav actually runs must find it.
    expect(el.querySelector('.listening')).not.toBeNull();
    render(null, el);
  });

  it('ActionButton appends .pending LAST, after the style modifier', () => {
    const el = document.createElement('div');
    const base = {
      modId: 'acme.tools',
      enabled: true,
      tr: ((_a: string, english: string) => english) as never,
      onToast: () => {},
    };
    render(
      <ActionButton {...base} item={{ command: 'acme.tools.run', label: 'Run' }} onRun={() => Promise.resolve(null)} />,
      el,
    );
    expect(el.querySelector('button')!.className).toBe('osf-btn osf-btn--sm');
    render(
      <ActionButton
        {...base}
        item={{ command: 'acme.tools.run', label: 'Wipe', style: 'danger' }}
        onRun={() => Promise.resolve(null)}
      />,
      el,
    );
    expect(el.querySelector('button')!.className).toBe('osf-btn osf-btn--sm osf-btn--danger');
    // A never-settling run leaves the button in the pending state.
    render(
      <ActionButton
        {...base}
        item={{ command: 'acme.tools.run', label: 'Wipe', style: 'danger' }}
        onRun={() => new Promise<string | null>(() => {})}
      />,
      el,
    );
    act(() => {
      el.querySelector('button')!.click();
    });
    expect(el.querySelector('button')!.className).toBe('osf-btn osf-btn--sm osf-btn--danger pending');
    render(null, el);
  });

  it('Overlay emits data-nav-modal="1" — the focus trap', () => {
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
    const el = await mount(seeded());

    const rows = el.querySelectorAll<HTMLElement>('#bindlist .kb-holder--list');
    expect(rows.length).toBeGreaterThan(0);
    for (const row of rows) {
      expect(row.tabIndex).toBe(0);
      expect(row.getAttribute('tabindex')).toBe('0');
    }

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
    const el = await mount(seeded());

    const dead = el.querySelectorAll<HTMLButtonElement>('#keyboard button.is-dead');
    expect(dead.length).toBe(1);
    for (const cell of dead) {
      expect(cell.disabled).toBe(true);
    }
    const punctuation = [...el.querySelectorAll<HTMLButtonElement>('#keyboard button')]
      .filter((c) => ['-', '=', '[', ']', '\\', ';', "'", ',', '.', '/'].includes(c.textContent!));
    expect(punctuation.length).toBe(10);
    for (const cell of punctuation) {
      expect(cell.classList.contains('is-dead')).toBe(false);
      expect(cell.disabled).toBe(false);
    }
    const esc = [...dead].find((c) => c.textContent === 'Esc');
    expect(esc).toBeDefined();
    expect(esc!.title).toBe('Reserved (cancels rebinds)');

    // Live cells are the inverse: enabled, not marked dead.
    const live = el.querySelectorAll<HTMLButtonElement>('#keyboard button:not(.is-dead)');
    expect(live.length).toBeGreaterThan(0);
    for (const cell of live) expect(cell.disabled).toBe(false);
  });

  it('a keyboard-labels map relabels cells; identities and liveness are unchanged', async () => {
    const plain = await mount(seeded());
    const plainCount = plain.querySelectorAll('#keyboard button').length;
    const plainNames = [...plain.querySelectorAll<HTMLButtonElement>('#keyboard button[data-name]')]
      .map((c) => c.dataset.name);
    render(null, plain);
    plain.remove();
    document.body.innerHTML = '';

    const el = await mount(seededGerman());
    const cellOf = (name: string) =>
      el.querySelector<HTMLButtonElement>(`#keyboard button[data-name="${name}"]`);

    // The labeled cells render the player's keycaps…
    expect(cellOf('Semicolon')!.querySelector('.kb-key-label')!.textContent).toBe('Ö');
    expect(cellOf('Grave')!.querySelector('.kb-key-label')!.textContent).toBe('^');
    expect(cellOf('Y')!.querySelector('.kb-key-label')!.textContent).toBe('Z');
    expect(cellOf('Minus')!.querySelector('.kb-key-label')!.textContent).toBe('ß');
    // …while unlabeled cells keep their authored US glyphs…
    expect(cellOf('Comma')!.querySelector('.kb-key-label')!.textContent).toBe(',');
    // …and every identity stays the canonical name (never a label).
    expect(cellOf('Semicolon')).not.toBeNull();
    expect(el.querySelector('#keyboard button[data-name="Ö"]')).toBeNull();

    const iso = cellOf('IntlBackslash')!;
    expect(iso).not.toBeNull();
    expect(iso.disabled).toBe(false);
    expect(iso.classList.contains('is-dead')).toBe(false);
    expect(el.querySelectorAll('#keyboard button').length).toBe(plainCount + 1);
    const germanNames = [...el.querySelectorAll<HTMLButtonElement>('#keyboard button[data-name]')]
      .map((c) => c.dataset.name)
      .filter((n) => n !== 'IntlBackslash');
    expect(germanNames).toEqual(plainNames);
  });

  it('an armed capture puts class="listening" in the document', async () => {
    const bridge = seeded();
    const el = await mount(bridge);

    expect(document.querySelector('.listening')).toBeNull();

    const rebind = el.querySelector<HTMLButtonElement>('#bindlist .osf-key');
    expect(rebind).not.toBeNull();
    rebind!.click();
    await flush();

    const listening = el.querySelector('.listening');
    expect(listening).not.toBeNull();
    // The button listens, and keeps its kit classes.
    expect(listening!.tagName).toBe('BUTTON');
    expect(listening!.className).toBe('osf-btn osf-btn--sm osf-key listening');

    expect(document.querySelectorAll('.listening').length).toBe(1);

    expect(bridge.requests[0]).toEqual({
      name: 'settings.captureKey',
      payload: { mod: 'osfui', key: 'toggleKey' },
    });

    bridge.settle(0, { armed: true, mod: 'osfui', key: 'toggleKey' });
    await flush();
    expect(document.querySelectorAll('.listening').length).toBe(1);

    bridge.deliver('settings.captured', {
      mod: 'osfui',
      key: 'toggleKey',
      name: 'F9',
      cancelled: false,
    });
    await flush();
    expect(document.querySelector('.listening')).toBeNull();
  });

  it('only the clicked instance listens when a binding is on screen twice', async () => {
    const el = await mount(seeded());

    // Select F10 so the mod binding renders in both the detail panel and the list.
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
