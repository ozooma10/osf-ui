// @vitest-environment jsdom
//
// harness.mockbridge.test.ts — the dev harness's mock bridge.
//
// The harness is a prediction tool: what it shows must be what the game does.
// The legacy mock had no tests at all, which is how it accumulated four silent
// failure modes (see mockbridge.ts's header). These cases lock down the ones
// that matter:
//
//   1. every supported command answers, and an unsupported one errors;
//   2. validation IS @lib/settings/normalize, not a look-alike;
//   3. an armed key capture can be disarmed;
//   4. persisted values round-trip through normalize on load.

import { afterEach, beforeEach, describe, expect, it, vi } from 'vitest';
import type { Setting } from '@sdk';
import { normalizeValue } from '@lib/settings/normalize';
import { installMock, validModId, type MockApi, type StorageLike } from '@harness/mockbridge';

// ---------------------------------------------------------------------------
// harness plumbing
// ---------------------------------------------------------------------------

interface Frame {
  type: string;
  payload: Record<string, unknown>;
  requestId?: string;
}

/** An in-memory Storage stand-in, so cases cannot leak into each other. */
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

/**
 * Every mock installed during a case. jsdom hands the whole FILE one `window`,
 * and an armed key capture holds a real keydown listener on it — so a case that
 * deliberately leaves a capture armed would otherwise preventDefault() the next
 * case's key events. Disarming them in afterEach is test hygiene, not a product
 * behaviour: in the browser each install gets a fresh page.
 */
let installed: MockApi[] = [];

function bridge(): { postMessage(json: string): void } {
  return (window as unknown as { osfui: { postMessage(json: string): void } }).osfui;
}

/** Send a ui.command the way the shared kit's `request()` does. */
function command(payload: Record<string, unknown>, requestId?: string): void {
  const envelope: Record<string, unknown> = { type: 'ui.command', payload };
  if (requestId) envelope['requestId'] = requestId;
  bridge().postMessage(JSON.stringify(envelope));
}

/** Drain queued macrotasks — the mock defers nearly every reply by design. */
async function settle(ms = 0): Promise<void> {
  await vi.advanceTimersByTimeAsync(ms);
}

function framesOf(type: string): Frame[] {
  return frames.filter((f) => f.type === type);
}

function lastOf(type: string): Frame | undefined {
  return framesOf(type).pop();
}

/**
 * Install a mock with the network-ish parts switched off: no real source load
 * (the fallback schema is seeded synchronously and is enough), no greeting, no
 * drag-drop wiring on the shared jsdom window.
 */
function install(storage: StorageLike | null = memStorage(), search = ''): MockApi {
  const api = installMock({ search, storage, autoLoad: false, greet: false, drop: false });
  // The mock calls `window.osfui.onMessage` for every native->web frame; the
  // shared kit owns that slot in the real page. Here it is the recorder.
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
  // (vitest's fs sandbox denies the ?raw reads of Version.h / UISettings.cpp
  // that the dev server allows). Both are correct behaviour and both would
  // bury the test output.
  vi.spyOn(console, 'log').mockImplementation(() => {});
  vi.spyOn(console, 'warn').mockImplementation(() => {});
  frames = [];
  installed = [];
  delete (window as unknown as { osfui?: unknown }).osfui;
  mock = install();
});

afterEach(() => {
  for (const api of installed) api.cancelCapture();
  vi.useRealTimers();
  vi.restoreAllMocks();
  vi.unstubAllGlobals();
});

// ---------------------------------------------------------------------------
// command coverage
// ---------------------------------------------------------------------------

describe('command coverage', () => {
  it('answers settings.get with the seeded registry', async () => {
    command({ command: 'settings.get' }, 'r1');
    await settle();
    const data = lastOf('settings.data');
    expect(data).toBeDefined();
    expect(data?.requestId).toBe('r1');
    const mods = data?.payload['mods'] as Array<{ id: string }>;
    expect(mods.map((m) => m.id)).toContain('osfui');
    // The game's own bindings ride along on settings.data, not a separate read.
    expect(data?.payload['vanillaKeys']).toBeInstanceOf(Array);
  });

  it('acks settings.set with the post-clamp value and pushes settings.changed', async () => {
    command({ command: 'settings.get' }); // subscribe first — pushes are gated on it
    await settle();
    command({ command: 'settings.set', mod: 'osfui', key: 'allowPanels', value: false }, 'r2');
    await settle();

    const ack = lastOf('settings.ack');
    expect(ack?.payload).toMatchObject({ mod: 'osfui', key: 'allowPanels', ok: true, value: false });
    expect(ack?.requestId).toBe('r2');
    expect(lastOf('settings.changed')?.payload).toMatchObject({
      mod: 'osfui',
      key: 'allowPanels',
      value: false,
    });
  });

  it('confirms the write-behind with settings.persisted ~500ms later', async () => {
    command({ command: 'settings.get' });
    await settle();
    command({ command: 'settings.set', mod: 'osfui', key: 'allowPanels', value: false });
    await settle();
    expect(lastOf('settings.persisted')).toBeUndefined();
    await settle(500);
    expect(lastOf('settings.persisted')?.payload).toEqual({ mod: 'osfui' });
  });

  it('re-sends the whole registry on settings.reset, with NO per-key fan-out', async () => {
    command({ command: 'settings.get' });
    await settle();
    command({ command: 'settings.set', mod: 'osfui', key: 'allowPanels', value: false });
    await settle();
    frames = [];

    command({ command: 'settings.reset', mod: 'osfui' }, 'r3');
    await settle();
    // Native parity (item 12): one authoritative settings.data, not N pushes.
    expect(framesOf('settings.changed')).toHaveLength(0);
    expect(lastOf('settings.data')?.requestId).toBe('r3');
    expect(mock.mods()[0]?.values['allowPanels']).toBe(true);
  });

  it('rejects settings.reset for an unknown mod instead of failing silently', async () => {
    command({ command: 'settings.reset', mod: 'nope.nope' }, 'r4');
    await settle();
    expect(lastOf('ui.result')?.payload).toMatchObject({ ok: false, code: 'unknown-setting' });
  });

  it('answers views.get, and menu.open on a fictional view marks it focused', async () => {
    mock.fixtures(true);
    frames = [];
    command({ command: 'views.get' }, 'r5');
    await settle();
    const ids = (lastOf('views.data')?.payload['views'] as Array<{ id: string }>).map((v) => v.id);
    expect(ids).toContain('acme.atlas/atlas');

    command({ command: 'menu.open', view: 'acme.shipworks/almanac' }, 'r6');
    await settle(500);
    const views = lastOf('views.data')?.payload['views'] as Array<{ id: string; focused: boolean }>;
    expect(views.find((v) => v.id === 'acme.shipworks/almanac')?.focused).toBe(true);
  });

  it('hides fictional views by default and shows them with ?fixtures=1', async () => {
    command({ command: 'views.get' });
    await settle();
    const ids = (lastOf('views.data')?.payload['views'] as Array<{ id: string }>).map((v) => v.id);
    expect(ids).not.toContain('acme.atlas/atlas');
    // The harness-only `fixture` marker must never reach a view: it is not a
    // field the runtime can produce.
    expect(lastOf('views.data')?.payload['views']).not.toContainEqual(
      expect.objectContaining({ fixture: expect.anything() }),
    );

    frames = [];
    delete (window as unknown as { osfui?: unknown }).osfui;
    install(memStorage(), '?fixtures=1');
    command({ command: 'views.get' });
    await settle();
    const withFixtures = (lastOf('views.data')?.payload['views'] as Array<{ id: string }>).map(
      (v) => v.id,
    );
    expect(withFixtures).toContain('acme.atlas/atlas');
  });

  it('answers hud.show / hud.hide and reconciles the catalog', async () => {
    mock.fixtures(true);
    command({ command: 'hud.hide', view: 'acme.shipworks/hudwidgets' }, 'r7');
    await settle(200);
    expect(lastOf('ui.result')?.payload).toMatchObject({ ok: true, command: 'hud.hide' });
    const views = lastOf('views.data')?.payload['views'] as Array<{ id: string; open: boolean }>;
    expect(views.find((v) => v.id === 'acme.shipworks/hudwidgets')?.open).toBe(false);
  });

  it('answers menu.close, defaulting `view` to the calling surface', async () => {
    mock.fixtures(true);
    mock.setSelfView('acme.shipworks/almanac');
    command({ command: 'menu.open', view: 'acme.shipworks/almanac' });
    await settle(500);
    frames = [];

    command({ command: 'menu.close' }, 'r8'); // no `view` — self
    await settle(200);
    expect(lastOf('ui.result')?.payload).toMatchObject({ ok: true, command: 'menu.close' });
    const views = lastOf('views.data')?.payload['views'] as Array<{ id: string; open: boolean }>;
    expect(views.find((v) => v.id === 'acme.shipworks/almanac')?.open).toBe(false);
  });

  it('answers i18n.get with an i18n.data catalog', async () => {
    command({ command: 'i18n.get', mod: 'osfui' }, 'r9');
    await settle();
    expect(lastOf('i18n.data')?.payload).toMatchObject({ mod: 'osfui', locale: 'en' });
  });

  it('refuses i18n.get for an id the store would refuse', async () => {
    command({ command: 'i18n.get', mod: 'Not A Mod Id' }, 'r10');
    await settle();
    expect(lastOf('ui.result')?.payload).toMatchObject({ ok: false, code: 'invalid-mod' });
  });

  it('answers game.get with a nested per-provider payload', async () => {
    command({ command: 'game.get' }, 'r11');
    await settle();
    expect(lastOf('game.data')?.payload['calendar']).toMatchObject({ available: true });
  });

  // The six commands below are real UiCommands that the LEGACY mock answered
  // with ui.error {unknown-command} — every one of them a silent harness gap.
  it('answers ping with runtime.pong', async () => {
    command({ command: 'ping' }, 'r12');
    await settle();
    expect(lastOf('runtime.pong')?.requestId).toBe('r12');
  });

  it('answers log', async () => {
    const spy = vi.spyOn(console, 'log').mockImplementation(() => {});
    command({ command: 'log', text: 'hello' }, 'r13');
    await settle();
    expect(lastOf('ui.result')?.payload).toMatchObject({ ok: true, command: 'log' });
    expect(spy).toHaveBeenCalledWith(expect.anything(), expect.anything(), 'hello');
  });

  it('answers setVisible with a ui.visibility edge', async () => {
    command({ command: 'setVisible', visible: false }, 'r14');
    await settle();
    expect(lastOf('ui.result')?.payload).toMatchObject({ ok: true, command: 'setVisible' });
    expect(lastOf('ui.visibility')?.payload).toEqual({ visible: false });
  });

  it('answers setViewHidden', async () => {
    command({ command: 'setViewHidden', hidden: true }, 'r15');
    await settle();
    expect(lastOf('ui.result')?.payload).toMatchObject({ ok: true, command: 'setViewHidden' });
  });

  it('answers osfui.gamepadRaw and osfui.handleBack', async () => {
    command({ command: 'osfui.gamepadRaw', raw: true }, 'r16');
    command({ command: 'osfui.handleBack', handle: true }, 'r17');
    await settle();
    const oks = framesOf('ui.result').map((f) => f.payload['command']);
    expect(oks).toContain('osfui.gamepadRaw');
    expect(oks).toContain('osfui.handleBack');
  });

  it('answers close', async () => {
    command({ command: 'close' }, 'r18');
    await settle();
    expect(lastOf('ui.result')?.payload).toMatchObject({ ok: true, command: 'close' });
  });

  it('acks a mod-namespaced plugin command, playing the bridge role', async () => {
    command({ command: 'acme.shipworks.doThing' }, 'r19');
    await settle(500);
    expect(lastOf('ui.result')?.payload).toMatchObject({
      ok: true,
      command: 'acme.shipworks.doThing',
    });
  });

  it('answers an unknown command with ui.error {unknown-command}', async () => {
    command({ command: 'totallyMadeUp' }, 'r20');
    await settle();
    const err = lastOf('ui.error');
    expect(err?.payload).toMatchObject({ code: 'unknown-command', command: 'totallyMadeUp' });
    expect(err?.requestId).toBe('r20');
  });

  it('treats a single-dot non-command as unknown, not as a plugin command', async () => {
    // "<author>.<modname>.<name>" needs TWO dots; one dot is a typo'd builtin.
    command({ command: 'settings.nope' }, 'r21');
    await settle();
    expect(lastOf('ui.error')?.payload).toMatchObject({ code: 'unknown-command' });
  });

  it('ignores malformed JSON rather than throwing', () => {
    expect(() => bridge().postMessage('{not json')).not.toThrow();
    expect(frames).toHaveLength(0);
  });
});

// ---------------------------------------------------------------------------
// injectors the legacy mock lacked entirely
// ---------------------------------------------------------------------------

describe('injectors', () => {
  it('fires ui.hotkey for the first key-typed setting', () => {
    expect(mock.hotkey()).toBe(true);
    expect(lastOf('ui.hotkey')?.payload).toEqual({ mod: 'osfui', key: 'toggleKey' });
  });

  it('fires a ui.gamepad down edge AND its release', async () => {
    mock.gamepad('LB');
    await settle();
    const pad = framesOf('ui.gamepad').map((f) => f.payload);
    // Without the release, @lib/lifecycle's edge tracker would never re-arm and
    // the button would work exactly once per page load.
    expect(pad).toEqual([
      { kind: 'button', button: { id: 0x0100, down: true } },
      { kind: 'button', button: { id: 0x0100, down: false } },
    ]);
  });

  it('fires ui.visibility on demand', () => {
    mock.visibility(false);
    expect(lastOf('ui.visibility')?.payload).toEqual({ visible: false });
  });
});

// ---------------------------------------------------------------------------
// validation parity
// ---------------------------------------------------------------------------

describe('validation delegates to @lib/settings/normalize', () => {
  // One table, two consumers: the module under test (through settings.set) and
  // the module it is supposed to be using. Any divergence — a clamp order, a
  // refusal, the maxLength quirk — fails here instead of silently making the
  // harness disagree with the game.
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
      // A single-setting schema whose key is the case's key, dropped in by the
      // same path a ?schema= or dropped file would take.
      const api = await freshWithSchema({
        id: 'acme.probe',
        groups: [{ settings: [c.setting] }],
      });
      command({ command: 'settings.set', mod: 'acme.probe', key: c.setting.key, value: c.value });
      await settle();

      const expected = normalizeValue(c.setting, c.value);
      const ack = lastOf('settings.ack')?.payload;
      if (expected === undefined) {
        expect(ack).toMatchObject({ ok: false, code: 'invalid-value' });
      } else {
        expect(ack).toMatchObject({ ok: true, value: expected });
        expect(api.mods().find((m) => m.id === 'acme.probe')?.values[c.setting.key]).toEqual(
          expected,
        );
      }
    });
  }
});

// ---------------------------------------------------------------------------
// key capture
// ---------------------------------------------------------------------------

describe('key capture', () => {
  it('resolves on a keydown and reports conflicts with the game bindings', async () => {
    command({ command: 'settings.captureKey', mod: 'osfui', key: 'toggleKey' }, 'cap1');
    await settle();
    expect(mock.captureArmed()).toBe(true);

    window.dispatchEvent(new KeyboardEvent('keydown', { key: 'F5', bubbles: true }));
    await settle();

    const captured = lastOf('settings.captured');
    expect(captured?.requestId).toBe('cap1'); // echoes the ARMING request
    expect(captured?.payload).toMatchObject({ name: 'F5', cancelled: false });
    // F5 is Starfield's Quicksave — the live-warn the view renders mid-capture.
    expect(captured?.payload['conflicts']).toContainEqual(
      expect.objectContaining({ mod: '@game', key: 'QuickSave' }),
    );
    expect(mock.captureArmed()).toBe(false);
  });

  it('cancels on Escape', async () => {
    command({ command: 'settings.captureKey', mod: 'osfui', key: 'toggleKey' }, 'cap2');
    await settle();
    window.dispatchEvent(new KeyboardEvent('keydown', { key: 'Escape', bubbles: true }));
    await settle();
    expect(lastOf('settings.captured')?.payload).toMatchObject({ name: '', cancelled: true });
  });

  it('refuses a second concurrent arm with capture-busy', async () => {
    command({ command: 'settings.captureKey', mod: 'osfui', key: 'toggleKey' }, 'cap3');
    await settle();
    command({ command: 'settings.captureKey', mod: 'osfui', key: 'toggleKey' }, 'cap4');
    await settle();
    expect(lastOf('ui.result')?.payload).toMatchObject({ ok: false, code: 'capture-busy' });
  });

  it('DISARMS on a click away — the legacy mock wedged every later capture here', async () => {
    command({ command: 'settings.captureKey', mod: 'osfui', key: 'toggleKey' }, 'cap5');
    // The pointer listener arms a macrotask late, so the click that STARTED the
    // capture (still propagating) cannot cancel it immediately.
    await settle();

    window.dispatchEvent(new Event('pointerdown', { bubbles: true }));
    await settle();

    expect(lastOf('settings.captured')?.payload).toMatchObject({ name: '', cancelled: true });
    expect(mock.captureArmed()).toBe(false);

    // The point of the fix: the NEXT capture still works.
    frames = [];
    command({ command: 'settings.captureKey', mod: 'osfui', key: 'toggleKey' }, 'cap6');
    await settle();
    expect(framesOf('ui.result')).toHaveLength(0); // no capture-busy
    expect(mock.captureArmed()).toBe(true);
  });

  it('exposes an explicit cancel path', async () => {
    command({ command: 'settings.captureKey', mod: 'osfui', key: 'toggleKey' }, 'cap7');
    await settle();
    expect(mock.cancelCapture()).toBe(true);
    expect(mock.cancelCapture()).toBe(false);
    expect(lastOf('settings.captured')?.payload).toMatchObject({ cancelled: true });
  });

  it('does not swallow a keypress once disarmed', async () => {
    command({ command: 'settings.captureKey', mod: 'osfui', key: 'toggleKey' }, 'cap8');
    await settle();
    mock.cancelCapture();
    frames = [];

    const e = new KeyboardEvent('keydown', { key: 'A', bubbles: true, cancelable: true });
    window.dispatchEvent(e);
    await settle();
    // Legacy left the keydown listener attached, so an unrelated later press was
    // preventDefault()ed and reported as a capture for a setting nobody was
    // editing.
    expect(e.defaultPrevented).toBe(false);
    expect(framesOf('settings.captured')).toHaveLength(0);
  });
});

// ---------------------------------------------------------------------------
// persistence
// ---------------------------------------------------------------------------

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
    // normalizeValue REFUSES (undefined) rather than coercing, so the schema
    // default is served — exactly what the store does with a bad values file.
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
    installMock({ search: '', storage: store, autoLoad: false, greet: false, drop: false });
    (window as unknown as { osfui: { onMessage: (j: string) => void } }).osfui.onMessage = (j) =>
      void frames.push(JSON.parse(j) as Frame);

    command({ command: 'settings.set', mod: 'osfui', key: 'allowPanels', value: false });
    await settle();
    expect(JSON.parse(store.getItem('osfui.mock.osfui') || '{}')).toMatchObject({
      allowPanels: false,
    });
  });
});

// ---------------------------------------------------------------------------
// mod id grammar
// ---------------------------------------------------------------------------

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

// ---------------------------------------------------------------------------

/**
 * Reinstall the mock with `schema` dropped in as an extra registered mod.
 *
 * The mock takes its registry from async sources, so a test that needs a
 * specific schema installs a fresh instance and upserts through the same
 * settings-source path (`?schema=<url>`) using a stubbed fetch — the point
 * being that nothing here reaches into mock internals a real caller could not.
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
    // autoLoad ON: this is the path a ?schema= URL actually takes.
    greet: false,
    drop: false,
  });
  (window as unknown as { osfui: { onMessage: (j: string) => void } }).osfui.onMessage = (j) =>
    void frames.push(JSON.parse(j) as Frame);
  installed.push(api);
  await api.loaded();
  frames = [];
  return api;
}
