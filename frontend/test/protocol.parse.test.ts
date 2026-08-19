// @vitest-environment jsdom

import { describe, it, expect, vi, beforeEach, afterEach } from 'vitest';
import { readFileSync } from 'node:fs';
import { resolve } from 'node:path';

const HELPER_SRC = readFileSync(resolve(process.cwd(), 'src/shared-kit/osfui.js'), 'utf8');

interface Frame {
  kind: string;
  name: string;
  id?: string;
  payload?: unknown;
}

interface Helper {
  ready: Promise<unknown>;
  send(name: string, payload?: Record<string, unknown>): boolean;
  request(name: string, payload?: Record<string, unknown>): Promise<unknown>;
  on(event: string, fn: (payload: unknown) => void): () => void;
  state: {
    get(key: string): unknown;
    on(key: string, fn: (value: unknown) => void): () => void;
  };
  i18n: {
    ready: Promise<{ locale: string; strings: Record<string, string> }>;
    readonly locale: string;
    t(address: string, english: string, vars?: Record<string, string | number>): string;
    localize(root?: ParentNode): void;
  };
  onMessage(json: string): void;
}

/** See protocol.envelope.test.ts for why this is `new Function`, not an import. */
function loadHelper(): { helper: Helper; sent: Frame[] } {
  const sent: Frame[] = [];
  (window as unknown as { osfui: unknown }).osfui = {
    postMessage(json: string) {
      sent.push(JSON.parse(json) as Frame);
    },
  };
  new Function(HELPER_SRC)();
  // sent[0] is the helper's own `osfui.hello`.
  return { helper: window.osfui as unknown as Helper, sent };
}

/** Deliver a native->web frame as the runtime does: as JSON text. */
function deliver(helper: Helper, message: unknown): void {
  helper.onMessage(JSON.stringify(message));
}

/** True when `promise` has not settled by the next microtask drain. */
async function stillPending(promise: Promise<unknown>): Promise<boolean> {
  const sentinel = Symbol('pending');
  return (await Promise.race([promise.then(() => 'settled' as const), Promise.resolve(sentinel)])) === sentinel;
}

let logged: unknown[][] = [];
let debugged: unknown[][] = [];

beforeEach(() => {
  logged = [];
  debugged = [];
  vi.spyOn(console, 'error').mockImplementation((...args: unknown[]) => void logged.push(args));
  vi.spyOn(console, 'debug').mockImplementation((...args: unknown[]) => void debugged.push(args));
});

afterEach(() => {
  vi.useRealTimers();
  vi.restoreAllMocks();
  document.documentElement.lang = '';
  document.body.innerHTML = '';
  window.localStorage.clear();
});

describe('frame parsing', () => {
  it('ignores malformed, typeless and unknown-kind frames without throwing', () => {
    const { helper } = loadHelper();
    let calls = 0;
    helper.on('ui.visibility', () => calls++);

    expect(() => helper.onMessage('{')).not.toThrow();
    expect(() => helper.onMessage('null')).not.toThrow();
    expect(() => helper.onMessage('[]')).not.toThrow();
    expect(() => helper.onMessage('{"payload":{}}')).not.toThrow();
    expect(() => deliver(helper, { kind: 42 })).not.toThrow();
    expect(() => deliver(helper, { kind: 'someday' })).not.toThrow();
    expect(calls).toBe(0);
  });

  it('ignores a 1.x envelope entirely', async () => {
    const { helper } = loadHelper();
    const seen: unknown[] = [];
    helper.on('ui.result', (p) => seen.push(p));
    helper.on('settings.data', (p) => seen.push(p));

    deliver(helper, { type: 'runtime.ready', payload: { version: '1.5.0' } });
    deliver(helper, { type: 'settings.data', payload: { mods: [] } });
    deliver(helper, { type: 'ui.result', requestId: 'q1', payload: { ok: true } });

    expect(seen).toEqual([]);
    expect(await stillPending(helper.ready)).toBe(true);
  });
});

describe('kind:"ready" — the answer to the page hello', () => {
  it('resolves the ready promise with the RuntimeInfo payload', async () => {
    const { helper } = loadHelper();

    deliver(helper, {
      kind: 'ready',
      payload: {
        game: 'Starfield',
        plugin: 'OSF UI',
        version: '2.0.0',
        bridgeVersion: '2.0',
        view: 'acme.mymod/dashboard',
        mod: 'acme.mymod',
      },
    });

    await expect(helper.ready).resolves.toMatchObject({
      version: '2.0.0',
      bridgeVersion: '2.0',
      view: 'acme.mymod/dashboard',
      mod: 'acme.mymod',
    });
  });

  it('resolves with {} when the ready frame carries no payload', async () => {
    const { helper } = loadHelper();
    deliver(helper, { kind: 'ready' });
    await expect(helper.ready).resolves.toEqual({});
  });

  it('triggers NO follow-up traffic — state arrives unasked', async () => {
    const { helper, sent } = loadHelper();

    deliver(helper, { kind: 'ready', payload: { version: '2.0.0' } });
    await helper.ready;

    expect(sent).toEqual([{ kind: 'send', name: 'osfui.hello', payload: {} }]);
  });

  it('keeps the first value when a second ready arrives', async () => {
    const { helper } = loadHelper();

    deliver(helper, { kind: 'ready', payload: { version: '2.0.0' } });
    deliver(helper, { kind: 'ready', payload: { version: '9.9.9' } });

    await expect(helper.ready).resolves.toMatchObject({ version: '2.0.0' });
  });
});

describe('kind:"state" — latest-wins values with replay', () => {
  it('delivers to subscribers and caches for state.get', () => {
    const { helper } = loadHelper();
    const seen: unknown[] = [];
    helper.state.on('osfui/views', (v) => seen.push(v));

    deliver(helper, { kind: 'state', mod: 'osfui', key: 'views', value: { views: [] } });

    expect(seen).toEqual([{ views: [] }]);
    expect(helper.state.get('osfui/views')).toEqual({ views: [] });
  });

  it('replays the current value SYNCHRONOUSLY on subscribe', () => {
    const { helper } = loadHelper();
    deliver(helper, { kind: 'state', mod: 'osfui', key: 'settings', value: { mods: [] } });

    let seen: unknown = 'never ran';
    helper.state.on('osfui/settings', (v) => {
      seen = v;
    });

    expect(seen).toEqual({ mods: [] });
  });

  it('replays only the LATEST value', () => {
    const { helper } = loadHelper();
    const seen: unknown[] = [];

    deliver(helper, { kind: 'state', mod: 'acme.mymod', key: 'fuel', value: 1 });
    deliver(helper, { kind: 'state', mod: 'acme.mymod', key: 'fuel', value: 2 });
    helper.state.on('acme.mymod/fuel', (v) => seen.push(v));
    deliver(helper, { kind: 'state', mod: 'acme.mymod', key: 'fuel', value: 3 });

    expect(seen).toEqual([2, 3]);
    expect(helper.state.get('acme.mymod/fuel')).toBe(3);
  });

  it('matches keys case-insensitively in both directions', () => {
    const { helper } = loadHelper();
    const seen: unknown[] = [];
    helper.state.on('ACME.MyMod/Ship', (v) => seen.push(v));

    deliver(helper, { kind: 'state', mod: 'acme.mymod', key: 'ship', value: 'Frontier' });

    expect(seen).toEqual(['Frontier']);
    expect(helper.state.get('acme.mymod/SHIP')).toBe('Frontier');
  });

  it('delivers a null value as a value', () => {
    const { helper } = loadHelper();
    const seen: unknown[] = [];
    helper.state.on('acme.mymod/target', (v) => seen.push(v));

    deliver(helper, { kind: 'state', mod: 'acme.mymod', key: 'target', value: null });

    expect(seen).toEqual([null]);
  });

  it('ignores a frame missing a string mod or key', () => {
    const { helper } = loadHelper();
    const seen: unknown[] = [];
    helper.state.on('osfui/settings', (v) => seen.push(v));

    deliver(helper, { kind: 'state', key: 'settings', value: { mods: [] } });
    deliver(helper, { kind: 'state', mod: 'osfui', value: { mods: [] } });
    deliver(helper, { kind: 'state', mod: 7, key: 'settings', value: { mods: [] } });

    // A half-addressed value would cache under a key nothing can subscribe to.
    expect(seen).toEqual([]);
    expect(helper.state.get('osfui/settings')).toBeUndefined();
  });

  it('isolates a throwing state handler and prints it', () => {
    const { helper } = loadHelper();
    const seen: string[] = [];

    helper.state.on('osfui/views', () => {
      seen.push('first');
      throw new Error('boom');
    });
    helper.state.on('osfui/views', () => seen.push('second'));

    deliver(helper, { kind: 'state', mod: 'osfui', key: 'views', value: {} });

    // A buggy view handler must not silence the rest of the page...
    expect(seen).toEqual(['first', 'second']);
    // ...and the author has to be able to find it.
    expect(String(logged[0]![0])).toContain('[osfui] state handler for "osfui/views" threw');
  });

  it('unsubscribes, and a replay-time throw does not escape subscribe', () => {
    const { helper } = loadHelper();
    let calls = 0;
    const off = helper.state.on('osfui/views', () => calls++);
    deliver(helper, { kind: 'state', mod: 'osfui', key: 'views', value: 1 });
    off();
    deliver(helper, { kind: 'state', mod: 'osfui', key: 'views', value: 2 });
    expect(calls).toBe(1);

    expect(() =>
      helper.state.on('osfui/views', () => {
        throw new Error('boom');
      }),
    ).not.toThrow();
  });

  it('requires a function handler', () => {
    const { helper } = loadHelper();
    expect(() => helper.state.on('osfui/views', undefined as unknown as () => void)).toThrow(
      TypeError,
    );
  });
});

describe('kind:"event" — one-shot happenings', () => {
  it('dispatches to on() subscribers', () => {
    const { helper } = loadHelper();
    const seen: unknown[] = [];
    helper.on('settings.changed', (p) => seen.push(p));

    deliver(helper, {
      kind: 'event',
      name: 'settings.changed',
      payload: { mod: 'm', key: 'k', value: 2 },
    });
    expect(seen).toEqual([{ mod: 'm', key: 'k', value: 2 }]);
  });

  it('coerces a missing payload to {} before handing it to subscribers', () => {
    const { helper } = loadHelper();
    const seen: unknown[] = [];
    helper.on('settings.persisted', (p) => seen.push(p));

    deliver(helper, { kind: 'event', name: 'settings.persisted' });
    expect(seen).toEqual([{}]);
  });

  it('ignores an unnamed event', () => {
    const { helper } = loadHelper();
    let calls = 0;
    helper.on('', () => calls++);
    expect(() => deliver(helper, { kind: 'event', payload: {} })).not.toThrow();
    expect(calls).toBe(0);
  });

  it('is NEVER replayed — a late subscriber hears nothing', () => {
    const { helper } = loadHelper();
    deliver(helper, { kind: 'event', name: 'ui.hotkey', payload: { mod: 'm', key: 'k' } });

    const seen: unknown[] = [];
    helper.on('ui.hotkey', (p) => seen.push(p));

    expect(seen).toEqual([]);
  });

  it('routes a mod event to its "<mod>.<name>" subscriber', () => {
    const { helper } = loadHelper();
    const seen: unknown[] = [];
    helper.on('acme.mymod.docked', (p) => seen.push(p));

    // Papyrus SendViewEvent / the C ABI's SendToWeb land here.
    deliver(helper, { kind: 'event', name: 'acme.mymod.docked', payload: { args: [1, 'Neon'] } });
    expect(seen).toEqual([{ args: [1, 'Neon'] }]);
  });

  it('isolates a throwing subscriber from the others and prints it', () => {
    const { helper } = loadHelper();
    const seen: string[] = [];

    helper.on('ui.visibility', () => {
      seen.push('first');
      throw new Error('boom');
    });
    helper.on('ui.visibility', () => seen.push('second'));

    deliver(helper, { kind: 'event', name: 'ui.visibility', payload: { visible: true } });

    expect(seen).toEqual(['first', 'second']);
    expect(String(logged[0]![0])).toContain('[osfui] event handler for "ui.visibility" threw');
  });

  it('snapshots the subscriber set, so unsubscribing mid-dispatch is not retroactive', () => {
    const { helper } = loadHelper();
    const seen: string[] = [];

    let offSecond = () => {};
    helper.on('ui.visibility', () => {
      seen.push('first');
      offSecond();
    });
    offSecond = helper.on('ui.visibility', () => seen.push('second'));

    deliver(helper, { kind: 'event', name: 'ui.visibility', payload: { visible: true } });

    expect(seen).toEqual(['first', 'second']);

    seen.length = 0;
    deliver(helper, { kind: 'event', name: 'ui.visibility', payload: { visible: false } });
    expect(seen).toEqual(['first']);
  });

  it('on() returns an unsubscribe that is idempotent, and requires a function', () => {
    const { helper } = loadHelper();
    let calls = 0;
    const off = helper.on('ui.hotkey', () => calls++);

    off();
    off();
    deliver(helper, { kind: 'event', name: 'ui.hotkey', payload: {} });
    expect(calls).toBe(0);

    expect(() => helper.on('ui.hotkey', undefined as unknown as () => void)).toThrow(TypeError);
  });
});

describe('the event and state channels never cross', () => {
  it('does not deliver state to an on() subscriber of the same name', () => {
    const { helper } = loadHelper();
    const seen: unknown[] = [];
    helper.on('osfui/settings', (p) => seen.push(p));
    helper.on('settings', (p) => seen.push(p));

    deliver(helper, { kind: 'state', mod: 'osfui', key: 'settings', value: { mods: [] } });
    expect(seen).toEqual([]);
  });

  it('does not deliver an event to a state.on() subscriber of the same name', () => {
    const { helper } = loadHelper();
    const seen: unknown[] = [];
    helper.state.on('settings.changed', (v) => seen.push(v));

    deliver(helper, { kind: 'event', name: 'settings.changed', payload: { mod: 'm' } });
    expect(seen).toEqual([]);
    expect(helper.state.get('settings.changed')).toBeUndefined();
  });
});

describe('OSF UI runtime-detected protocol faults arrive as osfui.debug.error and are PRINTED', () => {
  it('prints a wrong-endpoint-kind protocol fault with its detail object', () => {
    const { helper } = loadHelper();

    deliver(helper, {
      kind: 'event',
      name: 'osfui.debug.error',
      payload: {
        code: 'wrong-endpoint-kind',
        message: "'menu.open' is a request endpoint — use request(), not send()",
        detail: { name: 'menu.open' },
      },
    });

    expect(logged).toHaveLength(1);
    expect(String(logged[0]![0])).toBe(
      "[osfui] OSF UI runtime rejected wrong-endpoint-kind: 'menu.open' is a request endpoint — use request(), not send()",
    );
    expect(logged[0]![1]).toEqual({ name: 'menu.open' });
  });

  it('prints an unknown-endpoint protocol fault, falling back to the payload as detail', () => {
    const { helper } = loadHelper();

    deliver(helper, {
      kind: 'event',
      name: 'osfui.debug.error',
      payload: { code: 'unknown-endpoint', message: 'no such endpoint' },
    });

    expect(String(logged[0]![0])).toBe('[osfui] OSF UI runtime rejected unknown-endpoint: no such endpoint');
    expect(logged[0]![1]).toEqual({ code: 'unknown-endpoint', message: 'no such endpoint' });
  });

  it('degrades to "a message" when the protocol fault carries no code', () => {
    const { helper } = loadHelper();
    deliver(helper, { kind: 'event', name: 'osfui.debug.error', payload: {} });
    expect(String(logged[0]![0])).toBe('[osfui] OSF UI runtime rejected a message: ');
  });

  it('does NOT also dispatch it to on() subscribers', () => {
    const { helper } = loadHelper();
    const seen: unknown[] = [];
    helper.on('osfui.debug.error', (p) => seen.push(p));

    deliver(helper, {
      kind: 'event',
      name: 'osfui.debug.error',
      payload: { code: 'invalid-request', message: 'malformed message' },
    });

    expect(seen).toEqual([]);
    expect(logged).toHaveLength(1);
  });
});

describe('i18n rides the osfui/i18n state key', () => {
  it('adopts a catalog: locale, strings, <html lang>, t() and the DOM', async () => {
    const { helper } = loadHelper();
    document.body.innerHTML = '<span data-i18n="settings.title">Settings</span>';

    deliver(helper, {
      kind: 'state',
      mod: 'osfui',
      key: 'i18n',
      value: {
        mod: 'osfui',
        locale: 'pt-BR',
        strings: { 'settings.title': 'Configurações', 'settings.hi': 'Olá, {name}' },
      },
    });

    await expect(helper.i18n.ready).resolves.toMatchObject({ locale: 'pt-BR' });
    expect(helper.i18n.locale).toBe('pt-BR');
    expect(document.documentElement.lang).toBe('pt-BR');
    expect(helper.i18n.t('settings.title', 'Settings')).toBe('Configurações');
    expect(helper.i18n.t('settings.hi', 'Hello, {name}', { name: 'Sam' })).toBe('Olá, Sam');
    // Unknown address falls back to the authored English, still interpolated.
    expect(helper.i18n.t('nope', 'Bye, {name}', { name: 'Sam' })).toBe('Bye, Sam');
    expect(document.body.textContent).toBe('Configurações');
  });

  it('exposes locale as a PROPERTY, not a call', () => {
    const { helper } = loadHelper();
    expect(typeof helper.i18n.locale).toBe('string');
    expect(helper.i18n.locale).toBe('en');
  });

  it('falls back to locale "en" and an empty catalog on a malformed value', () => {
    const { helper } = loadHelper();
    deliver(helper, {
      kind: 'state',
      mod: 'osfui',
      key: 'i18n',
      value: { locale: 42, strings: 'nope' },
    });

    expect(helper.i18n.locale).toBe('en');
    expect(helper.i18n.t('settings.title', 'Settings')).toBe('Settings');
  });

  it('resolves i18n.ready even on a malformed value, so first paint is never blocked', async () => {
    const { helper } = loadHelper();
    deliver(helper, { kind: 'state', mod: 'osfui', key: 'i18n', value: null });

    await expect(helper.i18n.ready).resolves.toMatchObject({ locale: 'en' });
  });

  it('re-adopts a later catalog (a live locale change)', async () => {
    const { helper } = loadHelper();

    deliver(helper, {
      kind: 'state',
      mod: 'osfui',
      key: 'i18n',
      value: { locale: 'de', strings: { 'a.b': 'Hallo' } },
    });
    deliver(helper, {
      kind: 'state',
      mod: 'osfui',
      key: 'i18n',
      value: { locale: 'fr', strings: { 'a.b': 'Bonjour' } },
    });

    expect(helper.i18n.locale).toBe('fr');
    expect(helper.i18n.t('a.b', 'Hello')).toBe('Bonjour');
    // The promise keeps its first resolution; `locale` is the live value.
    await expect(helper.i18n.ready).resolves.toMatchObject({ locale: 'de' });
  });
});

describe('the osfui:trace flag', () => {
  it('logs nothing when it is unset', () => {
    const { helper } = loadHelper();
    helper.send('close');
    deliver(helper, { kind: 'event', name: 'ui.visibility', payload: { visible: true } });

    // Zero cost when off: `trace` is a no-op function chosen once at load.
    expect(debugged).toEqual([]);
  });

  it('logs every envelope in both directions when it is "1"', () => {
    window.localStorage.setItem('osfui:trace', '1');
    const { helper } = loadHelper();

    // Read at load, so the greeting itself is traced.
    expect(debugged[0]![0]).toBe('[osfui] ->');
    expect(debugged[0]![1]).toEqual({ kind: 'send', name: 'osfui.hello', payload: {} });

    helper.send('close');
    expect(debugged[1]![1]).toEqual({ kind: 'send', name: 'close', payload: {} });

    deliver(helper, { kind: 'state', mod: 'osfui', key: 'views', value: { views: [] } });
    expect(debugged[2]![0]).toBe('[osfui] <-');
    expect(debugged[2]![1]).toMatchObject({ kind: 'state', key: 'views' });

    deliver(helper, { kind: 'event', name: 'ui.visibility', payload: { visible: true } });
    expect(debugged[3]![1]).toMatchObject({ kind: 'event', name: 'ui.visibility' });
  });

  it('logs a settlement ONCE, with its latency', async () => {
    window.localStorage.setItem('osfui:trace', '1');
    const { helper, sent } = loadHelper();
    debugged.length = 0;

    const promise = helper.request('ping');
    deliver(helper, { kind: 'reply', id: sent[1]!.id!, payload: { pong: true } });
    await expect(promise).resolves.toEqual({ pong: true });

    expect(debugged).toHaveLength(2);
    expect(debugged[0]![0]).toBe('[osfui] ->');
    expect(debugged[1]![0]).toBe('[osfui] <-');
    expect(debugged[1]![1]).toMatchObject({ kind: 'reply', payload: { pong: true } });
    expect(String(debugged[1]![2])).toMatch(/^\d+ms$/);
  });

  it('treats any other value as off', () => {
    window.localStorage.setItem('osfui:trace', 'true');
    const { helper } = loadHelper();
    helper.send('close');
    expect(debugged).toEqual([]);
  });

  it('survives blocked browser storage', () => {
    const original = Object.getOwnPropertyDescriptor(window, 'localStorage');
    Object.defineProperty(window, 'localStorage', {
      configurable: true,
      get() {
        throw new Error('storage is blocked');
      },
    });
    try {
      const { helper, sent } = loadHelper();
      expect(sent[0]).toEqual({ kind: 'send', name: 'osfui.hello', payload: {} });
      helper.send('close');
      expect(debugged).toEqual([]);
    } finally {
      if (original) Object.defineProperty(window, 'localStorage', original);
      else delete (window as unknown as Record<string, unknown>).localStorage;
    }
  });
});
