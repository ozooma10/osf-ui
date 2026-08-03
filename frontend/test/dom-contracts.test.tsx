// @vitest-environment jsdom
//
// Pins the DOM shapes padnav.js queries. padnav ships verbatim and navigates by
// reading the DOM; it is not imported, bundled or type-checked, so nothing else
// catches a component that stops emitting the shape it queries. The symptom is a
// controller that silently cannot reach a control in game.
//
// If one of these fails, the fix is in the component, not in padnav.
//
// One caveat on framing. padnav reads classes in exactly two places, both
// order-blind: `el.closest(".row")` for banding (padnav.js:79) and
// `document.querySelector(".listening")` as a presence test (padnav.js:184). It
// never queries `.pending`. So the verbatim `className` comparisons on the kit's
// KeyField/ActionButton are not padnav contracts — they pin the kit's own cx()
// argument-order convention, which is what keeps these strings stable enough to
// assert at all. Kept in this file because it is where shipped class strings are
// pinned.

import { describe, it, expect, afterEach } from 'vitest';
import { render } from 'preact';
import { act } from 'preact/test-utils';
import { Row } from '@ui/Row';
import { Overlay } from '@ui/Overlay';
import { KeyField } from '@ui/KeyField';
import { ActionButton } from '@ui/ActionButton';
import { App } from '@views/osfui/keybinds/App';
import { nullBridge, type Bridge } from '@lib/bridge';
import type { RuntimeInfo, SettingsData } from '@sdk';

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
    // A fake that reports itself available must complete the handshake too:
    // nullBridge's `ready` rejects "no-bridge", which is the standalone case.
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
      // Subscribing IS the read: the seeded value replays synchronously, so the
      // board is painted on the first render with no lifecycle code at all.
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

/**
 * Let Preact settle. `act` is required: Preact schedules useEffect callbacks via
 * requestAnimationFrame, so without it the bridge subscriptions and the
 * document-level keydown listener are never installed and every push is dropped.
 * The inner timeout drains promise callbacks (the bridge request chains).
 */
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
  vanillaKeys: [{ event: 'QuickSave', title: 'Starfield (Quicksave)', name: 'F5' }],
} as unknown as SettingsData;

/** A bridge whose document already received the `osfui/settings` replay. */
const seeded = () => makeBridge({ 'osfui/settings': DATA });

/**
 * The same document plus a German keycap-label map (the additive `keyboard`
 * block a 2.x host publishes): the board must relabel cells — Ö on the
 * Semicolon position, the ISO `<` key appearing — while every identity
 * (data-name) and liveness stays exactly the no-map shape.
 */
const GERMAN_DATA: SettingsData = {
  ...DATA,
  keyboard: {
    layout: 'de-DE',
    labels: { Semicolon: 'Ö', Grave: '^', Y: 'Z', Z: 'Y', IntlBackslash: '<', Minus: 'ß' },
  },
} as unknown as SettingsData;
const seededGerman = () => makeBridge({ 'osfui/settings': GERMAN_DATA });

let host: HTMLElement | null = null;

async function mount(bridge: Bridge) {
  host = document.createElement('div');
  document.body.appendChild(host);
  // The initial render must be inside `act`, not merely followed by it: Preact
  // queues useEffect callbacks through `afterPaint`, and a render outside an act
  // scope leaves that queue unflushed, so the view mounts with none of its
  // bridge subscriptions installed.
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
    // padnav `bandOf` uses `el.closest(".row")` to decide whether two controls
    // count as one navigation line. No `.row`, no band.
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
    // The settings pane renders @ui/KeyField (keybinds has its own HolderRow
    // copy, asserted separately below). padnav only tests for the PRESENCE of
    // `.listening`, but className is compared verbatim here so the kit's
    // cx() argument order cannot drift unnoticed — that ordering is the whole
    // contract cx() documents.
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
    // Three orderings in one: base, base+modifier, base+modifier+state. A
    // reordered cx() call would change the string even though CSS matching
    // would not notice. NOTE: unlike `.listening`, padnav never queries
    // `.pending` — this pins the kit's cx() convention, not a padnav contract.
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
    // padnav queries `[data-nav-modal]` by attribute presence, so any value
    // traps; "1" matches the shipped markup.
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
    // padnav enumerates `button, input, select, textarea, a[href], [tabindex]`
    // and skips anything with `tabIndex < 0`. A click-to-select div is invisible
    // to it without an explicit tabindex.
    const el = await mount(seeded());

    const rows = el.querySelectorAll<HTMLElement>('#bindlist .kb-holder--list');
    expect(rows.length).toBeGreaterThan(0);
    for (const row of rows) {
      expect(row.tabIndex).toBe(0);
      expect(row.getAttribute('tabindex')).toBe('0');
    }

    // Detail-panel rows are not focusable: no row-level action, so making them
    // targets would add dead stops to the navigation path.
    // F5 specifically — the first live cell (F1) holds nothing and would render
    // the empty-state hint instead of any rows.
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
    // padnav skips `el.disabled || el.tabIndex < 0`. Without `disabled` the
    // arrow keys would stop on a cell that can never be bound.
    //
    // Esc is the only such cell now — the punctuation keys became bindable once
    // native learned their names — so this asserts the reserved-Esc contract
    // rather than a count.
    const el = await mount(seeded());

    const dead = el.querySelectorAll<HTMLButtonElement>('#keyboard button.is-dead');
    expect(dead.length).toBe(1);
    for (const cell of dead) {
      expect(cell.disabled).toBe(true);
    }
    // Punctuation keys are live cells: bindable, not skipped by padnav. Guards
    // against them silently going dead again.
    const punctuation = [...el.querySelectorAll<HTMLButtonElement>('#keyboard button')]
      .filter((c) => ['-', '=', '[', ']', '\\', ';', "'", ',', '.', '/'].includes(c.textContent!));
    expect(punctuation.length).toBe(10);
    for (const cell of punctuation) {
      expect(cell.classList.contains('is-dead')).toBe(false);
      expect(cell.disabled).toBe(false);
    }
    // Esc is resolvable natively, but the capture flow reads a press of it as
    // "cancel".
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

    // The ISO `<>` key exists exactly when the layout labels it: one extra
    // live cell over the ANSI board, nothing else moved.
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
    // padnav bails on `document.querySelector(".listening")`: all navigation
    // suspends while a rebind is armed, because the next key press belongs to
    // the capture.
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

    // Exactly one, even though the same binding may also be on screen in the
    // detail panel: the clicked button arms, not the binding.
    expect(document.querySelectorAll('.listening').length).toBe(1);

    // The capture went out as an ordinary request — not the 1.x open-ended one
    // with `timeoutMs: 0`. It settles in machine time ("armed"), and the key the
    // player eventually presses arrives separately as `settings.captured`.
    expect(bridge.requests[0]).toEqual({
      name: 'settings.captureKey',
      payload: { mod: 'osfui', key: 'toggleKey' },
    });

    // Which is what takes `.listening` back out of the document, so padnav
    // resumes: the reply alone never does.
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
