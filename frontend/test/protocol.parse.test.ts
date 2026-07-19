// @vitest-environment jsdom
//
// Inbound frame parsing, and the dispatch semantics the shipped helper
// documents in its own header (src/shared-kit/osfui.js:34-37):
//
//   "Replies that resolve a request() ALSO dispatch to on() subscribers (so one
//    render path can consume settings.data no matter who asked)"
//
// That fan-out is the reason the settings view can render from `settings.data`
// without caring whether it or another view asked for it. It reads like
// double-delivery; it is deliberate.

import { describe, it, expect, vi, afterEach } from 'vitest';
import { readFileSync } from 'node:fs';
import { resolve } from 'node:path';
import { parseMessage } from '@lib/protocol';

// Read from disk rather than importing: osfui.js is a classic script, not a
// module. Resolved against the vitest root (frontend/) because under the jsdom
// environment `import.meta.url` is an http: URL, not a file: one.
const HELPER_SRC = readFileSync(resolve(process.cwd(), 'src/shared-kit/osfui.js'), 'utf8');

interface Frame {
  type: string;
  requestId?: string;
  payload: Record<string, unknown>;
}

interface Helper {
  available(): boolean;
  send(command: string, fields?: Record<string, unknown>): boolean;
  request(
    command: string,
    fields?: Record<string, unknown>,
    opts?: { timeoutMs?: number },
  ): Promise<{ type: string; requestId?: string; payload: unknown }>;
  on(type: string, fn: (payload: unknown, message: unknown) => void): () => void;
  onMessage(json: string): void;
  ready: Promise<unknown>;
  i18nReady: Promise<{ locale: string; strings: Record<string, string> }>;
  locale(): string;
  t(address: string, english: string, vars?: Record<string, string | number>): string;
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
  return { helper: window.osfui as unknown as Helper, sent };
}

/** Deliver a native->web frame exactly as the runtime does: as JSON TEXT. */
function deliver(helper: Helper, message: unknown): void {
  helper.onMessage(JSON.stringify(message));
}

afterEach(() => {
  vi.useRealTimers();
  vi.restoreAllMocks();
});

describe('parseMessage — tolerance', () => {
  it('returns null for malformed JSON instead of throwing', () => {
    // The bridge is a one-way firehose: one corrupt frame must not kill the
    // view, so parse failures are swallowed (shared-kit/osfui.js:145 `catch { return; }`).
    expect(parseMessage('{')).toBeNull();
    expect(parseMessage('')).toBeNull();
    expect(parseMessage('undefined')).toBeNull();
  });

  it('returns null for JSON that is not an object', () => {
    expect(parseMessage('null')).toBeNull();
    expect(parseMessage('123')).toBeNull();
    expect(parseMessage('"settings.data"')).toBeNull();
    expect(parseMessage('true')).toBeNull();
  });

  it('returns null when `type` is missing or not a string', () => {
    expect(parseMessage('{}')).toBeNull();
    expect(parseMessage('{"type":5,"payload":{}}')).toBeNull();
    expect(parseMessage('{"type":null,"payload":{}}')).toBeNull();
    expect(parseMessage('{"type":["ui.result"],"payload":{}}')).toBeNull();
  });

  it('rejects arrays — not because they are arrays, but for want of a `type`', () => {
    // The title matters: there is no array check anywhere. `typeof [] ===
    // "object"` so an array survives the object guard and is rejected one line
    // later by `typeof m.type !== "string"` (protocol.ts:91). JSON offers no way
    // to give an array a named property, so every array is unusable in practice
    // — but assert the REASON, so a future `Array.isArray` "fix" is recognised
    // as a change in shape rather than a tidy-up.
    expect(parseMessage('[]')).toBeNull();
    expect(parseMessage('["ui.result"]')).toBeNull();
    expect(parseMessage('[{"type":"ui.result"}]')).toBeNull();
  });

  it('coerces a missing payload to {}', () => {
    // Subscribers are written against "payload is always an object"; the helper
    // guarantees it with `message.payload || {}` (shared-kit/osfui.js:173, :187).
    expect(parseMessage('{"type":"runtime.pong"}')).toEqual({
      type: 'runtime.pong',
      payload: {},
    });
  });

  it('coerces a null payload to {} as well', () => {
    // `?? {}` catches null too, matching the helper's `|| {}`.
    expect(parseMessage('{"type":"runtime.pong","payload":null}')?.payload).toEqual({});
  });

  it('passes a falsy-but-present payload through only when it is not nullish', () => {
    // `?? {}` (unlike the helper's `||`) keeps 0 / "" / false. A real runtime
    // never sends these — every documented payload is an object — but the two
    // implementations genuinely differ here, so pin the frontend one.
    expect(parseMessage('{"type":"x","payload":0}')?.payload).toBe(0);
    expect(parseMessage('{"type":"x","payload":false}')?.payload).toBe(false);
    expect(parseMessage('{"type":"x","payload":""}')?.payload).toBe('');
  });

  it('carries requestId only when it is a string', () => {
    const withId = parseMessage('{"type":"ui.result","requestId":"q3","payload":{"ok":true}}');
    expect(withId?.requestId).toBe('q3');

    // Same rule the helper applies before correlating
    // (shared-kit/osfui.js:168): a non-string id is treated as absent.
    for (const bad of ['5', 'null', 'true', '["q3"]']) {
      const m = parseMessage(`{"type":"ui.result","requestId":${bad},"payload":{}}`);
      expect(m).not.toBeNull();
      expect('requestId' in m!).toBe(false);
    }
  });

  it('drops unknown top-level keys — only type/payload/requestId survive', () => {
    const m = parseMessage('{"type":"ui.result","payload":{"ok":true},"extra":1,"seq":9}');
    expect(Object.keys(m!).sort()).toEqual(['payload', 'type']);
  });
});

describe('shipped helper — dispatch semantics', () => {
  it('settles the request promise AND fans out to on() subscribers', async () => {
    const { helper, sent } = loadHelper();
    const seen: Array<[unknown, unknown]> = [];
    helper.on('settings.data', (payload, message) => seen.push([payload, message]));

    const promise = helper.request('settings.get');
    const rid = sent[0]!.requestId!;

    const reply = { type: 'settings.data', requestId: rid, payload: { mods: [] } };
    deliver(helper, reply);

    // request() resolves with the whole MESSAGE, not just the payload — views
    // read `msg.payload` themselves.
    await expect(promise).resolves.toEqual(reply);

    // ...and the subscriber fired for the SAME frame. This is the documented
    // fan-out (shared-kit/osfui.js:34-36, :183): the settings view renders from
    // on("settings.data") regardless of who issued the settings.get.
    expect(seen).toHaveLength(1);
    expect(seen[0]![0]).toEqual({ mods: [] });
    expect(seen[0]![1]).toEqual(reply);
  });

  it('fans out to subscribers even when the reply REJECTS the request', async () => {
    const { helper, sent } = loadHelper();
    const errors: unknown[] = [];
    helper.on('ui.error', (payload) => errors.push(payload));

    const promise = helper.request('nope');
    const rid = sent[0]!.requestId!;
    deliver(helper, {
      type: 'ui.error',
      requestId: rid,
      payload: { code: 'unknown-command', message: 'no such command' },
    });

    await expect(promise).rejects.toMatchObject({ code: 'unknown-command' });
    // Rejection and dispatch are sequential, not exclusive: the reject path
    // falls through to the subscriber loop.
    expect(errors).toEqual([{ code: 'unknown-command', message: 'no such command' }]);
  });

  it('dispatches unsolicited pushes (no requestId) to subscribers', () => {
    const { helper } = loadHelper();
    const seen: unknown[] = [];
    helper.on('settings.changed', (p) => seen.push(p));

    deliver(helper, { type: 'settings.changed', payload: { mod: 'm', key: 'k', value: 2 } });
    expect(seen).toEqual([{ mod: 'm', key: 'k', value: 2 }]);
  });

  it('settles a pending request only ONCE; a repeat frame is subscriber-only', async () => {
    const { helper, sent } = loadHelper();
    let calls = 0;
    helper.on('ui.result', () => calls++);

    const promise = helper.request('close');
    const rid = sent[0]!.requestId!;
    const reply = { type: 'ui.result', requestId: rid, payload: { ok: true } };

    deliver(helper, reply);
    deliver(helper, reply);

    await expect(promise).resolves.toEqual(reply);
    // The pending entry is deleted on first settle, so the duplicate cannot
    // re-settle — but it still reaches subscribers.
    expect(calls).toBe(2);
  });

  it('treats a reply whose requestId matches NOTHING as an unsolicited push', () => {
    const { helper, sent } = loadHelper();
    const seen: unknown[] = [];
    helper.on('ui.result', (p) => seen.push(p));

    const promise = helper.request('close');
    const rid = sent[0]!.requestId!;

    // A stale id from a previous page, or a host echoing something we never
    // asked for. `const req = rid && pending.get(rid)` is falsy, so correlation
    // is skipped entirely and the frame falls through to the subscriber loop —
    // it must NOT settle the unrelated request that is still in flight.
    deliver(helper, { type: 'ui.result', requestId: 'q999', payload: { ok: true } });
    expect(seen).toEqual([{ ok: true }]);

    // The real reply still works afterwards; the stray frame consumed nothing.
    deliver(helper, { type: 'ui.result', requestId: rid, payload: { ok: true, command: 'close' } });
    return expect(promise).resolves.toMatchObject({ payload: { command: 'close' } });
  });

  it('does not correlate on an EMPTY requestId — it is treated as absent', () => {
    const { helper } = loadHelper();
    const seen: unknown[] = [];
    helper.on('ui.result', (p) => seen.push(p));

    // `rid && pending.get(rid)` short-circuits on "" before the map lookup
    // (shared-kit/osfui.js:169), mirroring native, which refuses to echo an
    // empty id at all (MessageBridge.cpp: `id.empty()` -> absent).
    deliver(helper, { type: 'ui.result', requestId: '', payload: { ok: true } });
    expect(seen).toEqual([{ ok: true }]);
  });

  it('coerces a missing payload to {} before handing it to subscribers', () => {
    const { helper } = loadHelper();
    const seen: unknown[] = [];
    helper.on('runtime.pong', (p) => seen.push(p));

    deliver(helper, { type: 'runtime.pong' });
    expect(seen).toEqual([{}]);
  });

  it('isolates a throwing subscriber from the others', () => {
    const { helper } = loadHelper();
    const spy = vi.spyOn(console, 'error').mockImplementation(() => {});
    const seen: string[] = [];

    helper.on('ui.visibility', () => {
      seen.push('first');
      throw new Error('boom');
    });
    helper.on('ui.visibility', () => seen.push('second'));

    deliver(helper, { type: 'ui.visibility', payload: { visible: true } });

    // A buggy view handler must not silence the rest of the page.
    expect(seen).toEqual(['first', 'second']);
    expect(spy).toHaveBeenCalled();
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

    deliver(helper, { type: 'ui.visibility', payload: { visible: true } });

    // `for (const fn of [...set])` iterates a COPY (shared-kit/osfui.js:186), so
    // a handler removed during this dispatch still runs this time.
    expect(seen).toEqual(['first', 'second']);

    seen.length = 0;
    deliver(helper, { type: 'ui.visibility', payload: { visible: false } });
    expect(seen).toEqual(['first']);
  });

  it('on() returns an unsubscribe that is idempotent', () => {
    const { helper } = loadHelper();
    let calls = 0;
    const off = helper.on('runtime.pong', () => calls++);

    off();
    off();
    deliver(helper, { type: 'runtime.pong', payload: {} });
    expect(calls).toBe(0);
  });

  it('ignores malformed and typeless frames without throwing', () => {
    const { helper } = loadHelper();
    let calls = 0;
    helper.on('ui.result', () => calls++);

    expect(() => helper.onMessage('{')).not.toThrow();
    expect(() => helper.onMessage('null')).not.toThrow();
    expect(() => helper.onMessage('{"payload":{}}')).not.toThrow();
    expect(calls).toBe(0);
  });
});

describe('shipped helper — the runtime.ready / i18n handshake', () => {
  it('resolves ready() and sends i18n.get UNCONDITIONALLY on runtime.ready', async () => {
    vi.useFakeTimers();
    const { helper, sent } = loadHelper();

    deliver(helper, {
      type: 'runtime.ready',
      payload: { game: 'Starfield', plugin: 'OSF UI', version: '1.0.0', bridgeVersion: '1.0' },
    });

    await expect(helper.ready).resolves.toMatchObject({ version: '1.0.0' });

    // No feature check, no capability negotiation: every bridge-bearing host is
    // expected to serve i18n.get, and hosts that do not simply refuse it fast
    // (shared-kit/osfui.js:149-153).
    expect(sent).toHaveLength(1);
    expect(sent[0]!.payload.command).toBe('i18n.get');
    expect(sent[0]!.requestId).toBe('q1');
  });

  it('re-requests the catalog on a REPEATED runtime.ready', async () => {
    vi.useFakeTimers();
    const { helper, sent } = loadHelper();

    deliver(helper, { type: 'runtime.ready', payload: { version: '1.0.0' } });
    deliver(helper, { type: 'runtime.ready', payload: { version: '1.0.0' } });

    // There is no once-guard: the handler fires per frame, so a second ready
    // sends a SECOND i18n.get with a fresh id. The `ready` promise is unaffected
    // (a Promise resolves once), and the duplicate reply is harmless — it just
    // re-adopts the same catalog. Pinned because a reload/reattach of the view
    // host is the realistic way a second ready arrives, and the extra round trip
    // is the shipped behaviour, not a bug to dedupe away.
    expect(sent).toHaveLength(2);
    expect(sent[0]!.payload.command).toBe('i18n.get');
    expect(sent[1]!.payload.command).toBe('i18n.get');
    expect(sent[0]!.requestId).toBe('q1');
    expect(sent[1]!.requestId).toBe('q2');

    await expect(helper.ready).resolves.toMatchObject({ version: '1.0.0' });
  });

  it('resolves ready() with {} when runtime.ready carries no payload', async () => {
    vi.useFakeTimers();
    const { helper } = loadHelper();
    deliver(helper, { type: 'runtime.ready' });
    // `resolveReady(message.payload || {})` — callers read `payload.version`
    // and would throw on undefined, so the coercion is load-bearing.
    await expect(helper.ready).resolves.toEqual({});
  });

  it('resolves i18nReady even when the i18n.get request FAILS', async () => {
    vi.useFakeTimers();
    const spy = vi.spyOn(console, 'error').mockImplementation(() => {});
    const { helper, sent } = loadHelper();

    deliver(helper, { type: 'runtime.ready', payload: {} });
    const rid = sent[0]!.requestId!;

    deliver(helper, {
      type: 'ui.error',
      requestId: rid,
      payload: { code: 'unknown-command', message: 'i18n.get is not supported' },
    });

    // Views await i18nReady before their first render; if a failure left it
    // pending they would hang forever on a host without localization (the dev
    // harness mock). It resolves with the CURRENT (English, empty) catalog.
    const catalog = await helper.i18nReady;
    expect(catalog.locale).toBe('en');
    expect(Object.keys(catalog.strings)).toEqual([]);
    expect(helper.locale()).toBe('en');

    // "unknown-command" is the expected refusal, so it is NOT logged as an error.
    expect(spy).not.toHaveBeenCalled();
  });

  it('logs — but still resolves i18nReady — on an UNEXPECTED i18n.get failure', async () => {
    vi.useFakeTimers();
    const spy = vi.spyOn(console, 'error').mockImplementation(() => {});
    const { helper, sent } = loadHelper();

    deliver(helper, { type: 'runtime.ready', payload: {} });
    deliver(helper, {
      type: 'ui.result',
      requestId: sent[0]!.requestId!,
      payload: { ok: false, code: 'internal', message: 'catalog read failed' },
    });

    await expect(helper.i18nReady).resolves.toMatchObject({ locale: 'en' });
    expect(spy).toHaveBeenCalledWith('OSF UI localization load failed:', expect.any(Error));
  });

  it('adopts an i18n.data catalog: locale, strings, <html lang> and t()', async () => {
    const { helper } = loadHelper();

    deliver(helper, {
      type: 'i18n.data',
      payload: {
        mod: 'osfui',
        locale: 'pt-BR',
        strings: { 'settings.title': 'Configurações', 'settings.hi': 'Olá, {name}' },
      },
    });

    await expect(helper.i18nReady).resolves.toMatchObject({ locale: 'pt-BR' });
    expect(helper.locale()).toBe('pt-BR');
    expect(document.documentElement.lang).toBe('pt-BR');
    expect(helper.t('settings.title', 'Settings')).toBe('Configurações');
    expect(helper.t('settings.hi', 'Hello, {name}', { name: 'Sam' })).toBe('Olá, Sam');
    // Unknown address falls back to the authored English, still interpolated.
    expect(helper.t('nope', 'Bye, {name}', { name: 'Sam' })).toBe('Bye, Sam');
  });

  it('falls back to locale "en" and an empty catalog on a malformed i18n.data', () => {
    const { helper } = loadHelper();
    deliver(helper, { type: 'i18n.data', payload: { locale: 42, strings: 'nope' } });

    expect(helper.locale()).toBe('en');
    expect(helper.t('settings.title', 'Settings')).toBe('Settings');
  });
});
