// @vitest-environment jsdom
//
// Failure classification and the BridgeError contract (bridge protocol 2.0).
// Three rules that look like details and are load-bearing:
//   1. Classification is by envelope KIND. `kind:"reply"` always resolves,
//      `kind:"error"` always rejects — a payload field can never be mistaken
//      for an outcome. (1.x had to inspect `ui.result { ok:false }`, which is
//      why a `settings.ack { ok:false }` was a *successful* request reporting a
//      rejected value. 2.0 gives that case its own kind: settings.set rejects.)
//   2. `err.code` is always a string, `""` when the error carried none, because
//      call sites branch with `e.code === "capture-busy"` and friends
//      (@lib/protocol `codeOf`).
//   3. Every failure an author can cause is printed to THIS page's console with
//      an `[osfui]` prefix — F12 DevTools is the debug surface, and a rejection
//      that only reached the SFSE log is a rejection the author never sees.

import { describe, it, expect, vi, beforeEach, afterEach } from 'vitest';
import { readFileSync } from 'node:fs';
import { resolve } from 'node:path';

// Read from disk rather than imported: osfui.js is a classic script, not a
// module. Resolved against the vitest root (frontend/) because under jsdom
// `import.meta.url` is an http: URL, not a file: one.
const HELPER_SRC = readFileSync(resolve(process.cwd(), 'src/shared-kit/osfui.js'), 'utf8');

interface Frame {
  kind: string;
  name: string;
  id?: string;
  payload?: unknown;
}

interface Helper {
  readonly available: boolean;
  ready: Promise<unknown>;
  send(name: string, payload?: Record<string, unknown>): boolean;
  request(
    name: string,
    payload?: Record<string, unknown>,
    // `number | undefined` rather than just optional: one case passes an
    // explicit undefined, which `exactOptionalPropertyTypes` would reject —
    // and that call is the path being pinned.
    opts?: { timeoutMs?: number | undefined },
  ): Promise<unknown>;
  papyrus: { request(name: string, ...args: unknown[]): Promise<unknown> };
  i18n: { ready: Promise<{ locale: string; strings: Record<string, string> }> };
  onMessage(json: string): void;
}

interface CaughtError extends Error {
  code?: unknown;
  payload?: unknown;
}

/** See protocol.envelope.test.ts for why this is `new Function`, not an import. */
function loadHelper(opts?: { bridge?: boolean }): { helper: Helper; sent: Frame[] } {
  const sent: Frame[] = [];
  const stub: Record<string, unknown> =
    opts?.bridge === false
      ? {}
      : {
          postMessage(json: string) {
            sent.push(JSON.parse(json) as Frame);
          },
        };
  (window as unknown as { osfui: unknown }).osfui = stub;
  new Function(HELPER_SRC)();
  // sent[0] is the helper's own `osfui.hello`; a request's id is sent[1].id.
  return { helper: window.osfui as unknown as Helper, sent };
}

function deliver(helper: Helper, message: unknown): void {
  helper.onMessage(JSON.stringify(message));
}

/**
 * Await a promise that must reject and hand back the error, typed. Call it
 * before advancing fake timers: it attaches its handler synchronously, which is
 * what stops a timeout rejection from surfacing as unhandled.
 */
async function caught(promise: Promise<unknown>): Promise<CaughtError> {
  try {
    await promise;
  } catch (e) {
    return e as CaughtError;
  }
  throw new Error('expected the request to reject');
}

/** Issue a request, deliver `payload` as its `kind:"error"`, return the rejection. */
function rejectionFor(payload: Record<string, unknown>): Promise<CaughtError> {
  const { helper, sent } = loadHelper();
  const promise = helper.request('demo.thing');
  deliver(helper, { kind: 'error', id: sent[1]!.id!, payload });
  return caught(promise);
}

/** Issue a request, deliver `payload` as its `kind:"reply"`, return the resolution. */
function resolutionFor(payload: unknown): Promise<unknown> {
  const { helper, sent } = loadHelper();
  const promise = helper.request('demo.thing');
  deliver(helper, { kind: 'reply', id: sent[1]!.id!, payload });
  return promise;
}

// Every failure path logs through the helper's `report`, so console noise is
// captured rather than printed — and inspected by the last describe.
let logged: unknown[][] = [];
let warned: unknown[][] = [];

beforeEach(() => {
  logged = [];
  warned = [];
  vi.spyOn(console, 'error').mockImplementation((...args: unknown[]) => void logged.push(args));
  vi.spyOn(console, 'warn').mockImplementation((...args: unknown[]) => void warned.push(args));
});

afterEach(() => {
  vi.useRealTimers();
  vi.restoreAllMocks();
});

describe('classification is by envelope KIND, not by payload content', () => {
  it('resolves a reply with the PAYLOAD, not the envelope', async () => {
    // 1.x resolved the whole message and every view dug out `msg.payload`.
    await expect(resolutionFor({ mod: 'm', key: 'k', value: 3 })).resolves.toEqual({
      mod: 'm',
      key: 'k',
      value: 3,
    });
  });

  it('resolves a reply even when its payload READS like a failure', async () => {
    // There is exactly one failure channel now. A handler that wants to reject
    // sends `kind:"error"`; `ok:false` in a reply is just data.
    await expect(resolutionFor({ ok: false, code: 'invalid-value' })).resolves.toEqual({
      ok: false,
      code: 'invalid-value',
    });
  });

  it('coerces a missing reply payload to {}', async () => {
    // `settle(id, true, message.payload || {})`. Callers destructure the
    // resolution; undefined would throw at the await site.
    await expect(resolutionFor(undefined)).resolves.toEqual({});
  });

  it('rejects on kind "error"', async () => {
    const err = await rejectionFor({ code: 'invalid-value', message: 'out of range' });
    expect(err).toBeInstanceOf(Error);
    expect(err.code).toBe('invalid-value');
  });
});

describe('BridgeError contract — code', () => {
  it('is "" (never undefined) when the error carries no code', async () => {
    const err = await rejectionFor({ message: 'it did not work' });

    // `err.code = p.code || ""`. Call sites do `e.code === "..."` and log
    // e.code; undefined would print "undefined".
    expect(err.code).toBe('');
    expect(err.code).not.toBeUndefined();
    expect(typeof err.code).toBe('string');
  });

  it('carries the error code verbatim when present', async () => {
    expect((await rejectionFor({ code: 'capture-busy', message: 'busy' })).code).toBe(
      'capture-busy',
    );
  });

  it('flattens a non-string code through `||` (empty string stays "")', async () => {
    // `p.code || ""` means a code of "" or 0 or null all collapse to "".
    expect((await rejectionFor({ code: '' })).code).toBe('');
    expect((await rejectionFor({ code: 0 })).code).toBe('');
  });

  it('surfaces the protocol-enforcement codes unchanged', async () => {
    // The codes the HOST can produce for a request, from MessageBridge.cpp.
    // Views branch on these, so they must arrive as themselves.
    for (const code of [
      'wrong-endpoint-kind',
      'unknown-endpoint',
      'invalid-request',
      'request-capacity',
      'no-response',
      'internal',
    ]) {
      expect((await rejectionFor({ code, message: 'x' })).code).toBe(code);
    }
  });
});

describe('BridgeError contract — message fallback chain', () => {
  it('prefers p.message', async () => {
    expect(
      (await rejectionFor({ code: 'invalid-value', message: 'out of range' })).message,
    ).toBe('out of range');
  });

  it('falls back to p.code when there is no message', async () => {
    expect((await rejectionFor({ code: 'capture-busy' })).message).toBe('capture-busy');
  });

  it('falls back to "request failed" when there is neither', async () => {
    expect((await rejectionFor({})).message).toBe('request failed');
  });
});

describe('BridgeError contract — payload', () => {
  it('attaches the error PAYLOAD on a host rejection', async () => {
    const { helper, sent } = loadHelper();
    const promise = helper.request('settings.captureKey', { mod: 'm', key: 'k' });
    deliver(helper, {
      kind: 'error',
      id: sent[1]!.id!,
      payload: { code: 'capture-busy', message: 'a capture is already armed' },
    });

    const err = await caught(promise);
    // The payload, not the envelope: the caller already holds the promise, so
    // the correlation id tells it nothing. `err.payload` is what DevTools shows
    // as an inspectable object next to the printed summary.
    expect(err.payload).toEqual({ code: 'capture-busy', message: 'a capture is already armed' });
  });

  it('is ABSENT on a timeout — the error is synthesised locally', async () => {
    vi.useFakeTimers();
    const { helper } = loadHelper();

    const pending = caught(helper.request('ping'));
    await vi.advanceTimersByTimeAsync(10000);
    const err = await pending;

    expect(err.code).toBe('timeout');
    expect(err.message).toBe('"ping" got no reply within 10000ms');
    // Not "present and undefined": the key is never assigned, so consumers can
    // use `"payload" in err` to tell a host refusal from a local give-up.
    expect('payload' in err).toBe(false);
  });

  it('is ABSENT on the no-bridge rejection', async () => {
    const { helper } = loadHelper({ bridge: false });
    expect(helper.available).toBe(false);

    const err = await caught(helper.request('ping'));
    expect(err.code).toBe('no-bridge');
    expect(err.message).toBe('no bridge (standalone preview)');
    expect('payload' in err).toBe(false);
  });
});

describe('no bridge — a plain-browser preview fails fast instead of hanging', () => {
  it('rejects ready with code "no-bridge"', async () => {
    const { helper } = loadHelper({ bridge: false });

    // 1.x left `ready` pending forever in a plain browser, so every view that
    // awaited it rendered nothing. A rejection is something a view can handle.
    const err = await caught(helper.ready);
    expect(err.code).toBe('no-bridge');
    expect(err.message).toBe('no bridge (standalone preview)');
  });

  it('reports the missing bridge as a NOTICE, not an authoring error', () => {
    loadHelper({ bridge: false });

    // Standalone preview is a supported way to run a view, so it warns; the
    // console.error sink is reserved for things the author should fix.
    expect(logged).toEqual([]);
    expect(warned).toHaveLength(1);
    expect(String(warned[0]![0])).toContain('[osfui] no bridge');
  });

  it('still resolves i18n.ready, so a view can render its inline English', async () => {
    const { helper } = loadHelper({ bridge: false });

    // Views await i18n.ready before their first paint; leaving it pending would
    // render a blank page in any plain browser.
    const catalog = await helper.i18n.ready;
    expect(catalog.locale).toBe('en');
    expect(Object.keys(catalog.strings)).toEqual([]);
  });

  it('makes a FRESH error per call, so a caller may annotate it safely', async () => {
    const { helper } = loadHelper({ bridge: false });
    const a = await caught(helper.request('ping'));
    const b = await caught(helper.request('ping'));
    expect(a).not.toBe(b);
  });
});

describe('the client timer', () => {
  it('defaults to 10000ms', async () => {
    vi.useFakeTimers();
    const { helper } = loadHelper();

    let settled = false;
    const promise = helper.request('game.get');
    const pending = caught(promise);
    void promise.then(
      () => (settled = true),
      () => (settled = true),
    );

    await vi.advanceTimersByTimeAsync(9999);
    expect(settled).toBe(false);

    await vi.advanceTimersByTimeAsync(1);
    expect((await pending).code).toBe('timeout');
  });

  it('honours an explicit timeoutMs', async () => {
    vi.useFakeTimers();
    const { helper } = loadHelper();

    const pending = caught(helper.request('game.get', undefined, { timeoutMs: 250 }));
    await vi.advanceTimersByTimeAsync(250);
    expect((await pending).message).toBe('"game.get" got no reply within 250ms');
  });

  it('gives papyrus.request a longer 15000ms timer', async () => {
    vi.useFakeTimers();
    const { helper } = loadHelper();

    // Papyrus answers over the VM's async call queue, which routinely takes
    // longer than a native handler; the platform default would time out a
    // healthy call.
    const pending = caught(helper.papyrus.request('GetWeight', 0x14));
    await vi.advanceTimersByTimeAsync(15000);
    expect((await pending).message).toBe('"papyrus.request" got no reply within 15000ms');
  });

  it('clears the timer once a reply lands, so a late tick cannot reject', async () => {
    vi.useFakeTimers();
    const { helper, sent } = loadHelper();

    const promise = helper.request('close');
    deliver(helper, { kind: 'reply', id: sent[1]!.id!, payload: { ok: true } });
    await expect(promise).resolves.toEqual({ ok: true });

    expect(vi.getTimerCount()).toBe(0);
    await vi.advanceTimersByTimeAsync(60000); // must not throw an unhandled rejection
  });

  it('drops the pending entry on timeout, so a late reply settles nothing', async () => {
    vi.useFakeTimers();
    const { helper, sent } = loadHelper();

    const pending = caught(helper.request('ping'));
    await vi.advanceTimersByTimeAsync(10000);
    expect((await pending).code).toBe('timeout');

    // The host is still free to answer — it does not know the page gave up.
    // The frame must be inert rather than resolving an already-rejected promise.
    expect(() =>
      deliver(helper, { kind: 'reply', id: sent[1]!.id!, payload: { pong: true } }),
    ).not.toThrow();
  });

  it('timeoutMs:0 disables only the CLIENT timer; the host still settles it', async () => {
    vi.useFakeTimers();
    const { helper, sent } = loadHelper();

    let settled = false;
    const promise = helper.request('acme.mymod.slow', undefined, { timeoutMs: 0 });
    const pending = caught(promise);
    void promise.then(
      () => (settled = true),
      () => (settled = true),
    );

    // No timer was even scheduled (`if (timeoutMs > 0)`).
    expect(vi.getTimerCount()).toBe(0);
    await vi.advanceTimersByTimeAsync(60 * 60 * 1000);
    await Promise.resolve();
    expect(settled).toBe(false);

    // This is no longer a hang: the host's own 30 s deadline (kRequestDeadline)
    // answers `no-response`, which is a DISTINCT code from `timeout` on purpose
    // — "the backend never answered" versus "the page gave up".
    deliver(helper, {
      kind: 'error',
      id: sent[1]!.id!,
      payload: { code: 'no-response', message: 'the backend never answered' },
    });
    expect((await pending).code).toBe('no-response');
  });

  it('treats a negative timeoutMs as "disabled" too', () => {
    vi.useFakeTimers();
    const { helper } = loadHelper();
    void helper.request('ping', undefined, { timeoutMs: -1 }).catch(() => {});
    // `if (timeoutMs > 0)` — anything <= 0 skips the timer entirely.
    expect(vi.getTimerCount()).toBe(0);
  });

  it('uses the 10000ms default when opts is present but has no timeoutMs key', () => {
    vi.useFakeTimers();
    const { helper } = loadHelper();

    // The default is chosen with `"timeoutMs" in opts`, so an explicit
    // `{ timeoutMs: undefined }` opts out of the default and — not being > 0 —
    // disables the client timer. Surprising, but it is what ships.
    void helper.request('ping', undefined, {}).catch(() => {});
    expect(vi.getTimerCount()).toBe(1);

    void helper.request('ping', undefined, { timeoutMs: undefined }).catch(() => {});
    expect(vi.getTimerCount()).toBe(1);
  });
});

describe('every failure reaches the page console with an [osfui] prefix', () => {
  it('prints a host rejection with the endpoint, the code, the message and the payload', async () => {
    await rejectionFor({ code: 'invalid-value', message: 'out of range' });

    expect(logged).toHaveLength(1);
    expect(String(logged[0]![0])).toBe(
      '[osfui] request "demo.thing" failed: invalid-value — out of range',
    );
    // The payload goes through as an OBJECT, not interpolated: DevTools expands
    // it, which is the whole reason this is console.error and not a log line.
    expect(logged[0]![1]).toEqual({ code: 'invalid-value', message: 'out of range' });
  });

  it('prints "(no code)" rather than an empty gap when the error carried none', async () => {
    await rejectionFor({ message: 'it did not work' });
    expect(String(logged[0]![0])).toBe(
      '[osfui] request "demo.thing" failed: (no code) — it did not work',
    );

    // ...and the message clause is dropped entirely when there is nothing to say.
    logged.length = 0;
    await rejectionFor({});
    expect(String(logged[0]![0])).toBe('[osfui] request "demo.thing" failed: (no code)');
  });

  it('prints a client timeout', async () => {
    vi.useFakeTimers();
    const { helper } = loadHelper();
    const pending = caught(helper.request('ping'));
    await vi.advanceTimersByTimeAsync(10000);
    await pending;

    expect(String(logged[0]![0])).toBe('[osfui] request "ping" failed: timeout after 10000ms');
  });

  it('prints the no-bridge rejection', async () => {
    const { helper } = loadHelper({ bridge: false });
    await caught(helper.request('ping'));

    expect(String(logged[0]![0])).toBe(
      '[osfui] request "ping" failed: no-bridge (standalone preview)',
    );
  });

  it('prints nothing on a successful request', async () => {
    await resolutionFor({ ok: true });
    expect(logged).toEqual([]);
  });
});
