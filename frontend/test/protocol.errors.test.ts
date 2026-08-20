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
  send(name: string, payload?: Record<string, unknown>): boolean;
  request(
    name: string,
    payload?: Record<string, unknown>,
    opts?: { timeoutMs?: number | undefined },
  ): Promise<unknown>;
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
    await expect(resolutionFor({ ok: false, code: 'invalid-value' })).resolves.toEqual({
      ok: false,
      code: 'invalid-value',
    });
  });

  it('coerces a missing reply payload to {}', async () => {
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
  it('attaches the error PAYLOAD on an OSF UI runtime rejection', async () => {
    const { helper, sent } = loadHelper();
    const promise = helper.request('settings.captureKey', { mod: 'm', key: 'k' });
    deliver(helper, {
      kind: 'error',
      id: sent[1]!.id!,
      payload: { code: 'capture-busy', message: 'a capture is already armed' },
    });

    const err = await caught(promise);
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
    expect('payload' in err).toBe(false);
  });

  it('is ABSENT on the no-bridge rejection', async () => {
    const { helper } = loadHelper({ bridge: false });

    const err = await caught(helper.request('ping'));
    expect(err.code).toBe('no-bridge');
    expect(err.message).toBe('no bridge (standalone preview)');
    expect('payload' in err).toBe(false);
  });
});

describe('no bridge — a plain-browser preview fails fast instead of hanging', () => {
  it('reports the missing bridge as a NOTICE, not an authoring error', () => {
    loadHelper({ bridge: false });

    expect(logged).toEqual([]);
    expect(warned).toHaveLength(1);
    expect(String(warned[0]![0])).toContain('[osfui] no bridge');
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

    expect(() =>
      deliver(helper, { kind: 'reply', id: sent[1]!.id!, payload: { pong: true } }),
    ).not.toThrow();
  });

  it('timeoutMs:0 disables only the CLIENT timer; the OSF UI runtime still settles it', async () => {
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

    deliver(helper, {
      kind: 'error',
      id: sent[1]!.id!,
      payload: { code: 'no-response', message: 'the endpoint handler never answered' },
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

    void helper.request('ping', undefined, {}).catch(() => {});
    expect(vi.getTimerCount()).toBe(1);

    void helper.request('ping', undefined, { timeoutMs: undefined }).catch(() => {});
    expect(vi.getTimerCount()).toBe(1);
  });
});

describe('every failure reaches the page console with an [osfui] prefix', () => {
  it('prints an OSF UI runtime rejection with the endpoint, the code, the message and the payload', async () => {
    await rejectionFor({ code: 'invalid-value', message: 'out of range' });

    expect(logged).toHaveLength(1);
    expect(String(logged[0]![0])).toBe(
      '[osfui] request "demo.thing" failed: invalid-value — out of range',
    );
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
