// @vitest-environment jsdom
//
// The dev harness's mock bridge (bridge protocol 2.0). The harness is a
// prediction tool: what it shows must be what the game does. Locks down the
// envelope grammar (kind enforcement, ids, the page-initiated handshake),
// endpoint coverage by KIND, that validation is @lib/settings/normalize itself
// rather than a look-alike, that an armed key capture can be disarmed, and that
// persisted values round-trip through normalize on load.

import { afterEach, beforeEach, describe, expect, it, vi } from 'vitest';
import { readFileSync } from 'node:fs';
import { resolve } from 'node:path';
import type { Setting } from '@sdk';
import { normalizeValue } from '@lib/settings/normalize';
import { installMock, validModId, type MockApi, type StorageLike } from '@devmock/mockbridge';

/** One native->web envelope, loosely typed so a case can assert on any field. */
interface Frame {
  kind: string;
  mod?: string;
  key?: string;
  value?: unknown;
  name?: string;
  id?: string;
  payload?: Record<string, unknown>;
}

/** In-memory Storage stand-in, so cases cannot leak into each other. */
function memStorage(seed: Record<string, string> = {}): StorageLike {
  const map = new Map(Object.entries(seed));
  return {
    getItem: (k) => (map.has(k) ? (map.get(k) as string) : null),
    setItem: (k, v) => void map.set(k, v),
    removeItem: (k) => void map.delete(k),
  };
}

let frames: Frame[] = [];
let mock: MockApi;
let seq = 0;

/**
 * Every mock installed during a case. jsdom shares one `window` across the file
 * and an armed key capture holds a real keydown listener on it, so a case that
 * leaves a capture armed would preventDefault() the next case's key events.
 * Disarming in afterEach is test hygiene: in the browser each install gets a
 * fresh page.
 */
let installed: MockApi[] = [];

function bridge(): { postMessage(json: string): void } {
  return (window as unknown as { osfui: { postMessage(json: string): void } }).osfui;
}

function post(envelope: Record<string, unknown>): void {
  bridge().postMessage(JSON.stringify(envelope));
}

/** A one-way message, the way the shared kit's `send()` posts it. */
function send(name: string, payload: Record<string, unknown> = {}): void {
  post({ kind: 'send', name, payload });
}

/** A correlated message; returns the id so the case can find its settlement. */
function request(name: string, payload: Record<string, unknown> = {}, id?: string): string {
  const rid = id || `q${++seq}`;
  post({ kind: 'request', name, id: rid, payload });
  return rid;
}

/**
 * Drain queued macrotasks. One virtual millisecond rather than zero on purpose:
 * the mock crosses a macrotask on the way in, so a `setTimeout(fn, 0)` a handler
 * schedules is scheduled DURING the tick, and a zero-length tick never reaches
 * it.
 */
async function settle(ms = 1): Promise<void> {
  await vi.advanceTimersByTimeAsync(ms);
}

/**
 * Run the handshake. `loaded()` first because `ready.version` is read out of
 * src/Core/Version.h and the greeting waits on it.
 */
async function greet(api: MockApi = mock): Promise<void> {
  await api.loaded();
  send('osfui.hello');
  await settle();
  await settle();
}

const eventsOf = (name: string) => frames.filter((f) => f.kind === 'event' && f.name === name);
const statesOf = (key: string) => frames.filter((f) => f.kind === 'state' && f.key === key);
const lastEvent = (name: string) => eventsOf(name).pop();
const lastState = (key: string) => statesOf(key).pop();
const replyTo = (id: string) => frames.filter((f) => f.kind === 'reply' && f.id === id).pop();
const errorTo = (id: string) => frames.filter((f) => f.kind === 'error' && f.id === id).pop();

/**
 * Install a mock with the network-ish parts off: no real source load (the
 * fallback schema is seeded synchronously) and no drag-drop wiring on the shared
 * jsdom window. Nothing is pushed until the document greets, so there is no
 * "quiet" option to pass any more.
 */
function install(storage: StorageLike | null = memStorage(), search = ''): MockApi {
  const api = installMock({ search, storage, autoLoad: false, drop: false });
  // The mock calls `window.osfui.onMessage` for every native->web frame; the
  // shared kit owns that slot in the real page, the recorder owns it here.
  (window as unknown as { osfui: { onMessage: (json: string) => void } }).osfui.onMessage = (
    json: string,
  ) => {
    frames.push(JSON.parse(json) as Frame);
  };
  installed.push(api);
  return api;
}

beforeEach(() => {
  vi.useFakeTimers();
  // The mock logs every frame and warns when a repo source is unreachable
  // (vitest's fs sandbox denies the ?raw reads of Version.h / UISettings.cpp the
  // dev server allows). Both are correct, and both would bury the test output.
  vi.spyOn(console, 'log').mockImplementation(() => {});
  vi.spyOn(console, 'warn').mockImplementation(() => {});
  frames = [];
  installed = [];
  seq = 0;
  delete (window as unknown as { osfui?: unknown }).osfui;
  mock = install();
});

afterEach(() => {
  for (const api of installed) api.cancelCapture();
  vi.useRealTimers();
  vi.restoreAllMocks();
  vi.unstubAllGlobals();
});

describe('handshake', () => {
  it('answers osfui.hello with ready, then the state replay, then events', async () => {
    await greet();

    const kinds = frames.map((f) => f.kind);
    expect(kinds[0]).toBe('ready');
    expect(frames[0]?.payload).toMatchObject({
      game: 'Starfield',
      plugin: 'OSF UI',
      bridgeVersion: '2.0',
      view: 'osfui/settings',
      mod: 'osfui',
    });
    // Every platform state key is replayed, so a view needs no read roundtrip
    // and nothing to re-request after F5.
    for (const key of ['settings', 'views', 'i18n']) {
      const state = lastState(key);
      expect(state, key).toBeDefined();
      expect(state?.mod, key).toBe('osfui');
    }
    // …and only then does the gate open.
    expect(kinds.indexOf('event')).toBeGreaterThan(kinds.lastIndexOf('state'));
    expect(lastEvent('ui.visibility')?.payload).toMatchObject({ visible: true });
  });

  it('pushes NOTHING before the page greets', async () => {
    mock.visibility(false);
    await settle(600);
    expect(frames).toHaveLength(0);
  });

  it('flushes the pre-greeting backlog after the state replay', async () => {
    // Events raised before the handshake are held (bounded at 64, oldest
    // dropped) so nothing is shouted at a page with no listeners — and then
    // DELIVERED by the greeting. That is the harness mirror of the native ABI's
    // message-before-first-paint guarantee: MessageBridge::HandleHello flushes
    // this queue rather than discarding it, covered by
    // tests/native/bridge_api_tests.cpp. Discarding here made the gate dead
    // code and hid every regression in that guarantee.
    for (let i = 0; i < 70; i++) mock.hotkey();
    await settle();
    expect(frames).toHaveLength(0);

    await greet();
    // Bounded to the newest 64 — an overflowing queue drops the OLDEST.
    expect(eventsOf('ui.hotkey')).toHaveLength(64);
    // …and they land BEHIND the replay, never ahead of it: a view's listener
    // is registered off the state it just received.
    const firstHotkey = frames.findIndex((f) => f.kind === 'event' && f.name === 'ui.hotkey');
    const lastSettings = frames.reduce(
      (last, f, i) => (f.kind === 'state' && f.key === 'settings' ? i : last),
      -1,
    );
    expect(lastSettings).toBeGreaterThanOrEqual(0);
    expect(firstHotkey).toBeGreaterThan(lastSettings);
    expect(lastState('settings')).toBeDefined();
  });

  it('re-greets a fresh document with a full replay', async () => {
    await greet();
    frames = [];
    // An F5: the same page greets again and must get a full replay.
    send('osfui.hello');
    await settle();
    await settle();
    expect(frames[0]?.kind).toBe('ready');
    expect(lastState('settings')).toBeDefined();
  });

  it('leaves the document ungreeted with greet:false', async () => {
    delete (window as unknown as { osfui?: unknown }).osfui;
    frames = [];
    const api = installMock({ storage: null, autoLoad: false, drop: false, greet: false });
    installed.push(api);
    (window as unknown as { osfui: { onMessage: (j: string) => void } }).osfui.onMessage = (j) =>
      void frames.push(JSON.parse(j) as Frame);

    await greet(api);
    expect(frames.some((f) => f.kind === 'ready')).toBe(false);
    expect(api.greeted()).toBe(false);
  });
});

describe('envelope grammar', () => {
  beforeEach(async () => {
    await greet();
    frames = [];
  });

  it('rejects a request that names a SEND endpoint', async () => {
    const id = request('close');
    await settle();
    expect(errorTo(id)?.payload).toMatchObject({ code: 'wrong-endpoint-kind' });
  });

  it('drops a send that names a REQUEST endpoint and reports a protocol fault', async () => {
    send('menu.open', { view: 'osfui/keybinds' });
    await settle(600);
    expect(frames.some((f) => f.kind === 'reply')).toBe(false);
    expect(lastEvent('osfui.debug.error')?.payload).toMatchObject({
      code: 'wrong-endpoint-kind',
    });
  });

  it('rejects an unknown request endpoint and reports an unknown send as a protocol fault', async () => {
    const id = request('totallyMadeUp');
    send('alsoMadeUp');
    await settle();
    expect(errorTo(id)?.payload).toMatchObject({ code: 'unknown-endpoint' });
    expect(lastEvent('osfui.debug.error')?.payload).toMatchObject({ code: 'unknown-endpoint' });
  });

  it('treats a single-dot name as a platform typo, not as a mod endpoint', async () => {
    const id = request('settings.nope');
    await settle(600);
    expect(errorTo(id)?.payload).toMatchObject({ code: 'unknown-endpoint' });
  });

  it('refuses a send carrying an id — it would expect a settlement that never comes', async () => {
    post({ kind: 'send', name: 'close', id: 'x1', payload: {} });
    await settle();
    expect(lastEvent('osfui.debug.error')?.payload).toMatchObject({ code: 'invalid-request' });
  });

  it('refuses a request with a missing or over-long id instead of demoting it', async () => {
    post({ kind: 'request', name: 'ping', payload: {} });
    post({ kind: 'request', name: 'ping', id: 'x'.repeat(65), payload: {} });
    await settle();
    expect(frames.some((f) => f.kind === 'reply')).toBe(false);
    expect(eventsOf('osfui.debug.error')).toHaveLength(2);
  });

  it('refuses an unknown kind and a non-object payload', async () => {
    post({ kind: 'shout', name: 'ping' });
    post({ kind: 'send', name: 'log', payload: 'text' });
    await settle();
    expect(eventsOf('osfui.debug.error').map((f) => f.payload?.['code'])).toEqual([
      'invalid-request',
      'invalid-request',
    ]);
  });

  it('ignores malformed JSON rather than throwing', async () => {
    expect(() => bridge().postMessage('{not json')).not.toThrow();
    await settle();
    expect(frames).toHaveLength(0);
  });
});

describe('settings', () => {
  beforeEach(async () => {
    await greet();
    frames = [];
  });

  it('publishes settings and game bindings on separate retained-state keys', async () => {
    send('osfui.hello');
    await settle();
    await settle();
    const value = lastState('settings')?.value as { mods: Array<{ id: string }> };
    expect(value.mods.map((m) => m.id)).toContain('osfui');
    expect(value).not.toHaveProperty('vanillaKeys');
    const keybindings = lastState('keybindings')?.value as { actions: unknown[] };
    expect(keybindings.actions).toBeInstanceOf(Array);
  });

  it('resolves settings.set with the post-clamp value and raises settings.changed', async () => {
    const id = request('settings.set', { mod: 'osfui', key: 'allowViews', value: false });
    await settle();
    expect(replyTo(id)?.payload).toEqual({ mod: 'osfui', key: 'allowViews', value: false });
    expect(lastEvent('settings.changed')?.payload).toMatchObject({
      mod: 'osfui',
      key: 'allowViews',
      value: false,
    });
  });

  it('REJECTS a refused settings.set instead of resolving an ok:false document', async () => {
    const id = request('settings.set', { mod: 'osfui', key: 'allowViews', value: 'yes' });
    const unknown = request('settings.set', { mod: 'osfui', key: 'nope', value: true });
    const missing = request('settings.set', { mod: 'osfui', key: 'allowViews' });
    await settle();
    expect(errorTo(id)?.payload).toMatchObject({ code: 'invalid-value' });
    expect(errorTo(unknown)?.payload).toMatchObject({ code: 'unknown-setting' });
    expect(errorTo(missing)?.payload).toMatchObject({ code: 'invalid-value' });
  });

  it('confirms the write-behind with settings.persisted ~500ms later', async () => {
    request('settings.set', { mod: 'osfui', key: 'allowViews', value: false });
    await settle();
    expect(lastEvent('settings.persisted')).toBeUndefined();
    await settle(500);
    expect(lastEvent('settings.persisted')?.payload).toEqual({ mod: 'osfui' });
  });

  it('re-publishes the whole registry on settings.reset, with NO per-key fan-out', async () => {
    request('settings.set', { mod: 'osfui', key: 'allowViews', value: false });
    await settle();
    frames = [];

    const id = request('settings.reset', { mod: 'osfui' });
    await settle();
    // Native parity: one authoritative state republish, not N per-key events.
    expect(eventsOf('settings.changed')).toHaveLength(0);
    expect(replyTo(id)?.payload).toEqual({});
    expect(lastState('settings')).toBeDefined();
    expect(mock.mods()[0]?.values['allowViews']).toBe(true);
  });

  it('rejects settings.reset for an unknown mod instead of failing silently', async () => {
    const id = request('settings.reset', { mod: 'nope.nope' });
    await settle();
    expect(errorTo(id)?.payload).toMatchObject({ code: 'unknown-setting' });
  });

  it('refuses a foreign-mod write from a view that is not a settings editor', async () => {
    mock.setSelfView('acme.shipworks/almanac');
    const id = request('settings.set', { mod: 'osfui', key: 'allowViews', value: false });
    const reset = request('settings.reset', { mod: 'osfui' });
    await settle();
    expect(errorTo(id)?.payload).toMatchObject({ code: 'forbidden' });
    expect(errorTo(reset)?.payload).toMatchObject({ code: 'forbidden' });
  });
});

describe('views', () => {
  beforeEach(async () => {
    await greet();
    frames = [];
  });

  /** The catalog carried by the latest `osfui/views` publish. */
  const catalog = () => (lastState('views')?.value as { views: Array<{ id: string }> }).views;

  it('hides fictional views until the sample-views toggle is on', async () => {
    mock.fixtures(false);
    await settle();
    expect(catalog().map((v) => v.id)).not.toContain('acme.atlas/atlas');
    expect(catalog().length).toBeGreaterThan(0);
    // The harness-only `fixture` marker must not reach a view: the runtime
    // cannot produce that field.
    expect(catalog()).not.toContainEqual(expect.objectContaining({ fixture: expect.anything() }));

    mock.fixtures(true);
    await settle();
    expect(catalog().map((v) => v.id)).toContain('acme.atlas/atlas');
  });

  it('marks a fictional view focused on menu.open and closed on menu.close', async () => {
    mock.fixtures(true);
    const open = request('menu.open', { view: 'acme.shipworks/almanac' });
    await settle(500);
    // The reply means "accepted and queued"; the open itself lands on the
    // reconcile that follows.
    expect(replyTo(open)?.payload).toEqual({});
    expect(
      (catalog() as Array<{ id: string; focused: boolean }>).find(
        (v) => v.id === 'acme.shipworks/almanac',
      )?.focused,
    ).toBe(true);

    // No `view` field — the request targets the calling view.
    mock.setSelfView('acme.shipworks/almanac');
    const close = request('menu.close');
    await settle(200);
    expect(replyTo(close)?.payload).toEqual({});
    expect(
      (catalog() as Array<{ id: string; open: boolean }>).find(
        (v) => v.id === 'acme.shipworks/almanac',
      )?.open,
    ).toBe(false);
  });

  it('rejects menu.open / menu.close for a view that was never discovered', async () => {
    const open = request('menu.open', { view: 'nope.nope/gone' });
    const close = request('menu.close', { view: 'nope.nope/gone' });
    await settle(500);
    expect(errorTo(open)?.payload).toMatchObject({ code: 'unknown-view' });
    expect(errorTo(close)?.payload).toMatchObject({ code: 'unknown-view' });
  });

  it('answers setViewHidden and osfui.setViewAutoStart', async () => {
    mock.fixtures(true);
    const hidden = request('setViewHidden', { hidden: true });
    await settle();
    expect(replyTo(hidden)?.payload).toEqual({});

    const bad = request('osfui.setViewAutoStart', { view: 'osfui/settings', enabled: true });
    await settle(200);
    // Menus are not player-configurable auto-start views.
    expect(errorTo(bad)?.payload).toMatchObject({ code: 'not-configurable' });
  });
});

describe('platform requests', () => {
  beforeEach(async () => {
    await greet();
    frames = [];
  });

  it('answers ping', async () => {
    const ping = request('ping');
    await settle();
    expect(replyTo(ping)?.payload).toEqual({});
  });

  it('answers the one-way sends without settling anything', async () => {
    send('close');
    send('osfui.gamepadRaw', { raw: true });
    send('osfui.handleBack', { handle: true });
    await settle();
    expect(frames.some((f) => f.kind === 'reply' || f.kind === 'error')).toBe(false);
    expect(eventsOf('osfui.debug.error')).toHaveLength(0);
  });

  it('turns setVisible into a ui.visibility edge', async () => {
    send('setVisible', { visible: false });
    await settle();
    expect(lastEvent('ui.visibility')?.payload).toMatchObject({ visible: false });
  });

  it('answers a mod-registered request and accepts a mod-registered send', async () => {
    const weight = request('acme.shipworks.getWeight');
    const action = request('acme.shipworks.doThing');
    send('acme.shipworks.tell');
    await settle(500);
    expect(replyTo(weight)?.payload).toEqual({ weight: 42.5 });
    expect(replyTo(action)?.payload).toMatchObject({ message: expect.any(String) });
    // A mod's own send endpoint is delivered, not surfaced as misuse.
    expect(eventsOf('osfui.debug.error')).toHaveLength(0);
  });

  it('answers papyrus.request the way a game with no listener does', async () => {
    const id = request('papyrus.request', { name: 'GetShipName', args: [] });
    send('papyrus.send', { name: 'Ping', args: [] });
    await settle();
    expect(errorTo(id)?.payload).toMatchObject({ code: 'papyrus-unavailable' });
    expect(eventsOf('osfui.debug.error')).toHaveLength(0);
  });
});

describe('i18n state', () => {
  it('publishes the catalog for the DOCUMENT`s own mod', async () => {
    await greet();
    expect(lastState('i18n')?.value).toMatchObject({ mod: 'osfui', locale: 'en' });

    frames = [];
    mock.setSelfView('acme.shipworks/almanac');
    await settle();
    expect(lastState('i18n')?.value).toMatchObject({ mod: 'acme.shipworks' });
  });

  it('re-publishes both registries and the catalog on a locale switch', async () => {
    await greet();
    frames = [];
    await mock.locale('pseudo');
    await settle();
    expect(lastState('i18n')?.value).toMatchObject({ locale: 'pseudo' });
    expect(lastState('settings')).toBeDefined();
    expect(lastState('views')).toBeDefined();
  });

});

describe('injectors', () => {
  beforeEach(async () => {
    await greet();
    frames = [];
  });

  it('fires ui.hotkey for the first key-typed setting', () => {
    expect(mock.hotkey()).toBe(true);
    expect(lastEvent('ui.hotkey')?.payload).toEqual({ mod: 'osfui', key: 'toggleKey' });
  });

  it('fires a ui.gamepad down edge AND its release', async () => {
    mock.gamepad('LB');
    await settle();
    const pad = eventsOf('ui.gamepad').map((f) => f.payload);
    // Without the release, @lib/lifecycle's edge tracker never re-arms and the
    // button works exactly once per page load.
    expect(pad).toEqual([
      { kind: 'button', button: { id: 0x0100, down: true } },
      { kind: 'button', button: { id: 0x0100, down: false } },
    ]);
  });

  it('fires ui.visibility on demand', () => {
    mock.visibility(false);
    expect(lastEvent('ui.visibility')?.payload).toMatchObject({ visible: false });
  });
});

describe('validation delegates to @lib/settings/normalize', () => {
  // One table, two consumers: the mock (through settings.set) and the module it
  // is supposed to be using. Any divergence — clamp order, a refusal, the
  // maxLength quirk — fails here instead of making the harness disagree with the
  // game.
  const CASES: Array<{ name: string; setting: Setting; value: unknown }> = [
    { name: 'bool accepts a boolean', setting: { key: 'b', type: 'bool' }, value: true },
    { name: 'bool refuses 1', setting: { key: 'b', type: 'bool' }, value: 1 },
    { name: 'bool refuses "true"', setting: { key: 'b', type: 'bool' }, value: 'true' },
    { name: 'int clamps to min', setting: { key: 'i', type: 'int', min: 1, max: 9 }, value: 0.4 },
    { name: 'int clamps to max', setting: { key: 'i', type: 'int', min: 1, max: 9 }, value: 40 },
    { name: 'int rounds', setting: { key: 'i', type: 'int' }, value: 2.6 },
    { name: 'float keeps precision', setting: { key: 'f', type: 'float' }, value: 2.6 },
    { name: 'float refuses NaN', setting: { key: 'f', type: 'float' }, value: NaN },
    { name: 'float refuses a string', setting: { key: 'f', type: 'float' }, value: '3' },
    {
      name: 'enum accepts a declared option',
      setting: { key: 'e', type: 'enum', options: ['a', 'b'] },
      value: 'b',
    },
    {
      name: 'enum refuses an undeclared option',
      setting: { key: 'e', type: 'enum', options: ['a', 'b'] },
      value: 'c',
    },
    {
      name: 'flags canonicalise to declared order and drop unknowns',
      setting: { key: 'g', type: 'flags', options: ['a', 'b', 'c'] },
      value: ['c', 'zz', 'a', 'a'],
    },
    { name: 'flags refuse a non-array', setting: { key: 'g', type: 'flags', options: ['a'] }, value: 'a' },
    { name: 'string caps at 256', setting: { key: 's', type: 'string' }, value: 'x'.repeat(300) },
    {
      name: 'string honours maxLength',
      setting: { key: 's', type: 'string', maxLength: 4 },
      value: 'abcdefg',
    },
    {
      name: 'string colour widget refuses a bad hex',
      setting: { key: 's', type: 'string', widget: 'color' },
      value: 'red',
    },
    {
      name: 'string colour widget accepts #rrggbbaa',
      setting: { key: 's', type: 'string', widget: 'color' },
      value: '#0a1b2c3d',
    },
    { name: 'key caps at 16 chars', setting: { key: 'k', type: 'key' }, value: 'K'.repeat(20) },
    { name: 'key refuses "" without allowUnbound', setting: { key: 'k', type: 'key' }, value: '' },
    {
      name: 'key accepts "" with allowUnbound',
      setting: { key: 'k', type: 'key', allowUnbound: true },
      value: '',
    },
  ];

  for (const c of CASES) {
    it(c.name, async () => {
      // Single-setting schema, dropped in by the same path a ?schema= or
      // dropped file takes.
      const api = await freshWithSchema({
        id: 'acme.probe',
        groups: [{ settings: [c.setting] }],
      });
      const id = request('settings.set', {
        mod: 'acme.probe',
        key: c.setting.key,
        value: c.value,
      });
      await settle();

      const expected = normalizeValue(c.setting, c.value);
      if (expected === undefined) {
        expect(errorTo(id)?.payload).toMatchObject({ code: 'invalid-value' });
      } else {
        expect(replyTo(id)?.payload).toMatchObject({ value: expected });
        expect(api.mods().find((m) => m.id === 'acme.probe')?.values[c.setting.key]).toEqual(
          expected,
        );
      }
    });
  }
});

describe('key capture', () => {
  beforeEach(async () => {
    await greet();
    frames = [];
  });

  it('settles the ARM in machine time and reports the key as an event', async () => {
    const id = request('settings.captureKey', { mod: 'osfui', key: 'toggleKey' });
    await settle();
    // The request answers "armed" immediately; a request left pending on a
    // person pressing a key would fight the client's own timeout.
    expect(replyTo(id)?.payload).toEqual({ armed: true, mod: 'osfui', key: 'toggleKey' });
    expect(mock.captureArmed()).toBe(true);

    window.dispatchEvent(new KeyboardEvent('keydown', { key: 'F5', bubbles: true }));
    await settle();

    const captured = lastEvent('settings.captured');
    expect(captured?.payload).toMatchObject({ name: 'F5', cancelled: false });
    // F5 is Starfield's Quicksave — the live warning the view renders mid-capture.
    expect(captured?.payload?.['conflicts']).toContainEqual(
      expect.objectContaining({ mod: '@game', key: 'QuickSave' }),
    );
    expect(mock.captureArmed()).toBe(false);
  });

  it('cancels on Escape', async () => {
    request('settings.captureKey', { mod: 'osfui', key: 'toggleKey' });
    await settle();
    window.dispatchEvent(new KeyboardEvent('keydown', { key: 'Escape', bubbles: true }));
    await settle();
    expect(lastEvent('settings.captured')?.payload).toMatchObject({ name: '', cancelled: true });
  });

  it('refuses a second concurrent arm with capture-busy', async () => {
    request('settings.captureKey', { mod: 'osfui', key: 'toggleKey' });
    await settle();
    const second = request('settings.captureKey', { mod: 'osfui', key: 'toggleKey' });
    await settle();
    expect(errorTo(second)?.payload).toMatchObject({ code: 'capture-busy' });
  });

  it('refuses a setting the schema did not declare rebindable', async () => {
    const id = request('settings.captureKey', { mod: 'osfui', key: 'allowViews' });
    await settle();
    expect(errorTo(id)?.payload).toMatchObject({ code: 'not-rebindable' });
    expect(mock.captureArmed()).toBe(false);
  });

  it('refuses a foreign-mod rebind from a view that is not a settings editor', async () => {
    mock.setSelfView('acme.shipworks/almanac');
    const id = request('settings.captureKey', { mod: 'osfui', key: 'toggleKey' });
    await settle();
    expect(errorTo(id)?.payload).toMatchObject({ code: 'forbidden' });
  });

  it('DISARMS on a click away — the legacy mock wedged every later capture here', async () => {
    request('settings.captureKey', { mod: 'osfui', key: 'toggleKey' });
    // The pointer listener arms a macrotask late, so the click that started the
    // capture (still propagating) cannot cancel it immediately.
    await settle();

    window.dispatchEvent(new Event('pointerdown', { bubbles: true }));
    await settle();

    expect(lastEvent('settings.captured')?.payload).toMatchObject({ name: '', cancelled: true });
    expect(mock.captureArmed()).toBe(false);

    // The next capture still arms.
    frames = [];
    const again = request('settings.captureKey', { mod: 'osfui', key: 'toggleKey' });
    await settle();
    expect(errorTo(again)).toBeUndefined(); // no capture-busy
    expect(mock.captureArmed()).toBe(true);
  });

  it('exposes an explicit cancel path', async () => {
    request('settings.captureKey', { mod: 'osfui', key: 'toggleKey' });
    await settle();
    expect(mock.cancelCapture()).toBe(true);
    expect(mock.cancelCapture()).toBe(false);
    expect(lastEvent('settings.captured')?.payload).toMatchObject({ cancelled: true });
  });

  it('does not swallow a keypress once disarmed', async () => {
    request('settings.captureKey', { mod: 'osfui', key: 'toggleKey' });
    await settle();
    mock.cancelCapture();
    frames = [];

    const e = new KeyboardEvent('keydown', { key: 'A', bubbles: true, cancelable: true });
    window.dispatchEvent(e);
    await settle();
    // Disarming must detach the keydown listener: a stale one preventDefault()s
    // an unrelated later press and reports it as a capture for a setting nobody
    // is editing.
    expect(e.defaultPrevented).toBe(false);
    expect(eventsOf('settings.captured')).toHaveLength(0);
  });
});

describe('persisted values round-trip through normalize on load', () => {
  it('clamps an out-of-range persisted number', async () => {
    const api = await freshWithSchema(
      { id: 'acme.probe', groups: [{ settings: [{ key: 'n', type: 'int', min: 0, max: 10 }] }] },
      { 'osfui.mock.acme.probe': JSON.stringify({ n: 999 }) },
    );
    expect(api.mods().find((m) => m.id === 'acme.probe')?.values['n']).toBe(10);
  });

  it('falls back to the default when the persisted value is of the wrong type', async () => {
    const api = await freshWithSchema(
      {
        id: 'acme.probe',
        groups: [{ settings: [{ key: 'b', type: 'bool', default: true }] }],
      },
      { 'osfui.mock.acme.probe': JSON.stringify({ b: 'yes' }) },
    );
    // normalizeValue returns undefined rather than coercing, so the schema
    // default is served — what the store does with a bad values file.
    expect(api.mods().find((m) => m.id === 'acme.probe')?.values['b']).toBe(true);
  });

  it('canonicalises a persisted flags array', async () => {
    const api = await freshWithSchema(
      {
        id: 'acme.probe',
        groups: [{ settings: [{ key: 'g', type: 'flags', options: ['a', 'b', 'c'] }] }],
      },
      { 'osfui.mock.acme.probe': JSON.stringify({ g: ['c', 'a', 'nope'] }) },
    );
    expect(api.mods().find((m) => m.id === 'acme.probe')?.values['g']).toEqual(['a', 'c']);
  });

  it('writes committed values back under the mod id', async () => {
    const store = memStorage();
    delete (window as unknown as { osfui?: unknown }).osfui;
    frames = [];
    const api = installMock({ search: '', storage: store, autoLoad: false, drop: false });
    installed.push(api);
    (window as unknown as { osfui: { onMessage: (j: string) => void } }).osfui.onMessage = (j) =>
      void frames.push(JSON.parse(j) as Frame);

    request('settings.set', { mod: 'osfui', key: 'allowViews', value: false });
    await settle();
    expect(JSON.parse(store.getItem('osfui.mock.osfui') || '{}')).toMatchObject({
      allowViews: false,
    });
  });
});

describe('the shipped shared kit talks to it end to end', () => {
  it('greets, replays state, and settles a mod request through osfui.request()', async () => {
    // The kit is the contract third-party views use; a mock the kit cannot drive
    // is a mock that predicts nothing.
    const source = readFileSync(resolve(process.cwd(), 'src/shared-kit/osfui.js'), 'utf8');
    window.eval(source); // takes over onMessage and sends osfui.hello itself

    const helper = (
      window as unknown as {
        osfui: {
          ready: Promise<Record<string, unknown>>;
          request(name: string): Promise<Record<string, unknown>>;
          state: { get(key: string): unknown };
        };
      }
    ).osfui;

    await mock.loaded();
    await settle();
    await settle();

    await expect(helper.ready).resolves.toMatchObject({
      plugin: 'OSF UI',
      bridgeVersion: '2.0',
      view: 'osfui/settings',
      mod: 'osfui',
    });
    // The replay populated the kit's state cache — no read roundtrip anywhere.
    expect(helper.state.get('osfui/settings')).toBeDefined();
    expect(helper.state.get('osfui/views')).toBeDefined();

    // `request()` resolves the reply PAYLOAD, not an envelope.
    const waiting = helper.request('acme.shipworks.getWeight');
    await settle(10);
    await expect(waiting).resolves.toEqual({ weight: 42.5 });
  });
});

describe('validModId', () => {
  it('accepts the reserved dotless built-in and <author>.<modname>', () => {
    expect(validModId('osfui')).toBe(true);
    expect(validModId('acme.shipworks')).toBe(true);
  });

  it('refuses other dotless ids, uppercase, and over-long ids', () => {
    expect(validModId('osf')).toBe(false);
    expect(validModId('Acme.Shipworks')).toBe(false);
    expect(validModId('a'.repeat(60) + '.' + 'b'.repeat(10))).toBe(false);
    expect(validModId('a.b.c')).toBe(false);
  });
});

/**
 * Reinstall the mock with `schema` dropped in as an extra registered mod, then
 * greet it.
 *
 * The registry comes from async sources, so this installs a fresh instance and
 * upserts through the same settings-source path (`?schema=<url>`) with a stubbed
 * fetch, touching no mock internals a real caller could not reach.
 */
async function freshWithSchema(
  schema: Record<string, unknown>,
  seed: Record<string, string> = {},
): Promise<MockApi> {
  delete (window as unknown as { osfui?: unknown }).osfui;
  frames = [];
  vi.stubGlobal('fetch', async (url: string) => {
    if (String(url).includes('probe.json')) {
      return { ok: true, json: async () => schema } as unknown as Response;
    }
    return { ok: false, json: async () => ({}) } as unknown as Response;
  });
  const api = installMock({
    search: '?schema=probe.json',
    storage: memStorage(seed),
    // autoLoad on: the path a ?schema= URL takes.
    drop: false,
  });
  (window as unknown as { osfui: { onMessage: (j: string) => void } }).osfui.onMessage = (j) =>
    void frames.push(JSON.parse(j) as Frame);
  installed.push(api);
  await greet(api);
  frames = [];
  return api;
}
