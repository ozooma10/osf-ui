// @vitest-environment jsdom
//
// Failure classification and the BridgeError contract.
//
// Two rules here are easy to "fix" into breakage and must not be:
//   1. ONLY `ui.error` and `ui.result { ok:false }` are failures. A
//      `settings.ack { ok:false }` is a successful REQUEST that reports a
//      rejected value — callers inspect it, they do not catch it.
//   2. `err.code` is always a string, `""` when the reply carried none, because
//      call sites branch with `e.code === "capture-busy"` and friends.

import { describe, it, expect, vi, afterEach } from 'vitest';
import { readFileSync } from 'node:fs';
import { resolve } from 'node:path';
import { isFailureReply, isBridgeError } from '@lib/protocol';
import type { BridgeEnvelope } from '@sdk';

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
  request(
    command: string,
    fields?: Record<string, unknown>,
    // `number | undefined` (not just optional) on purpose: one test passes an
    // EXPLICIT undefined, which `exactOptionalPropertyTypes` would otherwise
    // reject — and that call is exactly the surprising path being pinned.
    opts?: { timeoutMs?: number | undefined },
  ): Promise<unknown>;
  onMessage(json: string): void;
}

interface CaughtError extends Error {
  code?: unknown;
  reply?: unknown;
}

/** See protocol.envelope.test.ts for why this is `new Function`, not an import. */
function loadHelper(opts?: { bridge?: boolean }): { helper: Helper; sent: Frame[] } {
  const sent: Frame[] = [];
  // Omitting postMessage models a view WITHOUT permissions.nativeBridge (or a
  // plain browser): `available()` is `typeof g.postMessage === "function"`.
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
  return { helper: window.osfui as unknown as Helper, sent };
}

function deliver(helper: Helper, message: unknown): void {
  helper.onMessage(JSON.stringify(message));
}

/**
 * Await a promise that must REJECT and hand back the error, typed.
 *
 * Call it BEFORE advancing fake timers: it attaches its handler synchronously,
 * which is what stops a timeout rejection from surfacing as unhandled.
 */
async function caught(promise: Promise<unknown>): Promise<CaughtError> {
  try {
    await promise;
  } catch (e) {
    return e as CaughtError;
  }
  throw new Error('expected the request to reject');
}

/** Issue a request, deliver `payload` as its reply, return the rejection. */
function rejectionFor(payload: Record<string, unknown>, type = 'ui.result'): Promise<CaughtError> {
  const { helper, sent } = loadHelper();
  const promise = helper.request('demo.thing');
  deliver(helper, { type, requestId: sent[0]!.requestId!, payload });
  return caught(promise);
}

afterEach(() => {
  vi.useRealTimers();
  vi.restoreAllMocks();
});

const env = (type: string, payload: unknown): BridgeEnvelope =>
  ({ type, payload }) as BridgeEnvelope;

describe('isFailureReply', () => {
  it('is true for ui.error regardless of payload', () => {
    expect(isFailureReply(env('ui.error', { code: 'unknown-command', message: 'x' }))).toBe(true);
    expect(isFailureReply(env('ui.error', {}))).toBe(true);
    expect(isFailureReply(env('ui.error', null))).toBe(true);
  });

  it('is true for ui.result with ok:false', () => {
    expect(isFailureReply(env('ui.result', { ok: false, code: 'capture-busy' }))).toBe(true);
  });

  it('is false for ui.result with ok:true', () => {
    expect(isFailureReply(env('ui.result', { ok: true, command: 'close' }))).toBe(false);
  });

  it('requires ok === false EXACTLY — a merely falsy ok is not a failure', () => {
    // The helper compares `p.ok === false` (shared-kit/osfui.js:174), so a
    // malformed 0/""/null/absent `ok` RESOLVES. Preserved: a host that ever
    // shipped a sloppy ok would otherwise start throwing at existing views.
    expect(isFailureReply(env('ui.result', { ok: 0 }))).toBe(false);
    expect(isFailureReply(env('ui.result', { ok: null }))).toBe(false);
    expect(isFailureReply(env('ui.result', { ok: 'false' }))).toBe(false);
    expect(isFailureReply(env('ui.result', {}))).toBe(false);
  });

  it('is false for a null/undefined ui.result payload', () => {
    expect(isFailureReply(env('ui.result', null))).toBe(false);
    expect(isFailureReply(env('ui.result', undefined))).toBe(false);
  });

  it('is false for EVERY typed reply, including settings.ack { ok:false }', () => {
    const typed: Array<[string, unknown]> = [
      ['runtime.ready', { version: '1.0.0' }],
      ['runtime.pong', {}],
      ['game.data', { calendar: { available: false } }],
      ['views.data', { views: [] }],
      ['i18n.data', { mod: 'osfui', locale: 'en', strings: {} }],
      ['settings.data', { mods: [] }],
      // A rejected VALUE, not a failed request: settings.set resolves and the
      // caller reads ack.ok / ack.code to show "invalid value" inline.
      ['settings.ack', { mod: 'm', key: 'k', ok: false, code: 'invalid-value' }],
      ['settings.changed', { mod: 'm', key: 'k', value: 1 }],
      ['settings.persisted', { mod: 'm' }],
      // Likewise `cancelled` capture: the request succeeded, the user pressed Esc.
      ['settings.captured', { mod: 'm', key: 'k', name: '', cancelled: true }],
      ['ui.hotkey', { mod: 'm', key: 'k' }],
      ['ui.visibility', { visible: false }],
      ['ui.gamepad', { kind: 'button', button: { id: 0, down: true } }],
    ];
    for (const [type, payload] of typed) {
      expect(isFailureReply(env(type, payload)), type).toBe(false);
    }
  });
});

describe('BridgeError contract — code', () => {
  it('is "" (never undefined) when the reply carries no code', async () => {
    const err = await rejectionFor({ ok: false, message: 'it did not work' });

    // `err.code = p.code || ""` (shared-kit/osfui.js:176). Call sites do
    // `e.code === "..."` and log e.code; undefined would print "undefined".
    expect(err.code).toBe('');
    expect(err.code).not.toBeUndefined();
    expect(typeof err.code).toBe('string');
    expect(isBridgeError(err)).toBe(true);
  });

  it('carries the reply code verbatim when present', async () => {
    const err = await rejectionFor({ ok: false, code: 'capture-busy', message: 'busy' });
    expect(err.code).toBe('capture-busy');
  });

  it('flattens a non-string code through `||` (empty string stays "")', async () => {
    // `p.code || ""` means a code of "" or 0 or null all collapse to "".
    expect((await rejectionFor({ ok: false, code: '' })).code).toBe('');
    expect((await rejectionFor({ ok: false, code: 0 })).code).toBe('');
  });
});

describe('BridgeError contract — message fallback chain', () => {
  it('prefers p.message', async () => {
    const err = await rejectionFor({ ok: false, code: 'invalid-value', message: 'out of range' });
    expect(err.message).toBe('out of range');
  });

  it('falls back to p.code when there is no message', async () => {
    const err = await rejectionFor({ ok: false, code: 'capture-busy' });
    expect(err.message).toBe('capture-busy');
  });

  it('falls back to "request failed" when there is neither', async () => {
    const err = await rejectionFor({ ok: false });
    expect(err.message).toBe('request failed');
  });

  it('applies the same chain to ui.error', async () => {
    expect((await rejectionFor({ code: 'unknown-command' }, 'ui.error')).message).toBe(
      'unknown-command',
    );
    expect((await rejectionFor({}, 'ui.error')).message).toBe('request failed');
  });
});

describe('BridgeError contract — reply', () => {
  it('attaches the WHOLE reply message on a reply-driven rejection', async () => {
    const { helper, sent } = loadHelper();
    const promise = helper.request('settings.captureKey', { mod: 'm', key: 'k' });
    const rid = sent[0]!.requestId!;
    const reply = { type: 'ui.result', requestId: rid, payload: { ok: false, code: 'capture-busy' } };
    deliver(helper, reply);

    const err = await caught(promise);
    // The message, not the payload — views inspect err.reply.payload and can
    // correlate on err.reply.requestId.
    expect(err.reply).toEqual(reply);
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
    // use `"reply" in err` to distinguish a real refusal from a local giving-up.
    expect('reply' in err).toBe(false);
  });

  it('is ABSENT on the no-bridge rejection', async () => {
    const { helper } = loadHelper({ bridge: false });
    expect(helper.available()).toBe(false);

    const err = await caught(helper.request('ping'));
    expect(err.code).toBe('no-bridge');
    expect(err.message).toBe('no bridge (standalone preview)');
    expect('reply' in err).toBe(false);
  });
});

describe('shipped helper — timeouts', () => {
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
    deliver(helper, { type: 'ui.result', requestId: sent[0]!.requestId!, payload: { ok: true } });
    await expect(promise).resolves.toMatchObject({ type: 'ui.result' });

    expect(vi.getTimerCount()).toBe(0);
    await vi.advanceTimersByTimeAsync(60000); // must not throw an unhandled rejection
  });

  it('timeoutMs: 0 DISABLES the timeout — and leaks the pending entry', async () => {
    vi.useFakeTimers();
    const { helper, sent } = loadHelper();

    // settings.captureKey is the reason this exists: it waits on the user
    // pressing a key and has no legitimate deadline (bridge.ts RequestOptions).
    let settled = false;
    const promise = helper.request(
      'settings.captureKey',
      { mod: 'm', key: 'k' },
      { timeoutMs: 0 },
    );
    void promise.then(
      () => (settled = true),
      () => (settled = true),
    );

    // No timer was even scheduled (`if (timeoutMs > 0)`).
    expect(vi.getTimerCount()).toBe(0);
    await vi.advanceTimersByTimeAsync(60 * 60 * 1000);
    await Promise.resolve();
    expect(settled).toBe(false);

    // DOCUMENTED LEAK, asserted as-is rather than idealised: nothing ever
    // removes the pending entry, so if the reply never comes the closure (and
    // its resolve/reject) is retained for the life of the page. The observable
    // proof is that an arbitrarily late reply STILL correlates and settles.
    const rid = sent[0]!.requestId!;
    deliver(helper, {
      type: 'settings.captured',
      requestId: rid,
      payload: { mod: 'm', key: 'k', name: 'F9', cancelled: false },
    });
    await expect(promise).resolves.toMatchObject({ payload: { name: 'F9' } });
  });

  it('treats a negative timeoutMs as "disabled" too', async () => {
    vi.useFakeTimers();
    const { helper } = loadHelper();
    void helper.request('ping', undefined, { timeoutMs: -1 }).catch(() => {});
    // `if (timeoutMs > 0)` — anything <= 0 skips the timer entirely.
    expect(vi.getTimerCount()).toBe(0);
  });

  it('uses the 10000ms default when opts is present but has no timeoutMs key', async () => {
    vi.useFakeTimers();
    const { helper } = loadHelper();

    // The default is chosen with `"timeoutMs" in opts`, so an EXPLICIT
    // `{ timeoutMs: undefined }` opts out of the default and — being neither
    // > 0 — disables the timeout. Surprising, but it is what ships.
    void helper.request('ping', undefined, {}).catch(() => {});
    expect(vi.getTimerCount()).toBe(1);

    void helper.request('ping', undefined, { timeoutMs: undefined }).catch(() => {});
    expect(vi.getTimerCount()).toBe(1);
  });
});
