// @vitest-environment jsdom
//
// Outbound envelope shape: what the shipped helper (src/shared-kit/osfui.js,
// bridge protocol 2.0) posts, and what src/runtime/MessageBridge.cpp accepts.
//
// 2.0 moved routing metadata BESIDE the payload — `kind`, `name` and `id` are
// envelope fields, so no payload key can steer routing any more (1.x carried
// `payload.command`). The OSF UI runtime's envelope validator is mirrored at the bottom
// of this file, executable rather than prose, and every envelope this helper
// can produce is swept through it.
//
// jsdom is needed because the file drives the real helper (an IIFE decorating
// `window.osfui`).

import { describe, it, expect } from 'vitest';
import { readFileSync } from 'node:fs';
import { resolve } from 'node:path';

// Read from disk, not imported: osfui.js is a classic script. Resolved against
// the vitest root (frontend/) because under jsdom `import.meta.url` is an http:
// URL, not a file: one.
const HELPER_SRC = readFileSync(resolve(process.cwd(), 'src/shared-kit/osfui.js'), 'utf8');

/** One web->native envelope. Loosely typed so a case can assert on any field. */
interface Frame {
  kind: string;
  name: string;
  id?: string;
  payload?: unknown;
}

interface Helper {
  readonly available: boolean;
  send(name: string, payload?: Record<string, unknown>): boolean;
  request(
    name: string,
    payload?: Record<string, unknown>,
    opts?: { timeoutMs?: number },
  ): Promise<unknown>;
  markReady(): boolean;
  papyrus: {
    float(value: number): { $papyrus: 'float'; value: number };
    call(script: string, fn: string, ...args: unknown[]): boolean;
    send(name: string, ...args: unknown[]): boolean;
    request(name: string, ...args: unknown[]): Promise<unknown>;
  };
}

/**
 * Install a fake native bridge and evaluate the shipped helper over it.
 *
 * `new Function` rather than import: osfui.js is a classic script whose
 * `window` / `document` / `setTimeout` references must bind to jsdom's globals.
 * A fresh `window.osfui` per call resets the helper's private `seq` closure, so
 * request ids are deterministic per test.
 *
 * `sent[0]` is ALWAYS the `osfui.hello` greeting: the helper greets during its
 * own evaluation (see "the handshake is page-initiated" below), so a case that
 * inspects its own traffic indexes from 1.
 */
function loadHelper(opts?: { bridge?: boolean }): { helper: Helper; raw: string[]; sent: Frame[] } {
  const raw: string[] = [];
  const sent: Frame[] = [];
  // Omitting postMessage models a view without a native bridge (a plain
  // browser): `available` is `typeof g.postMessage === "function"`.
  const stub: Record<string, unknown> =
    opts?.bridge === false
      ? {}
      : {
          postMessage(json: string) {
            raw.push(json);
            sent.push(JSON.parse(json) as Frame);
          },
        };
  (window as unknown as { osfui: unknown }).osfui = stub;
  new Function(HELPER_SRC)();
  return { helper: window.osfui as unknown as Helper, raw, sent };
}

describe('the handshake is page-initiated — hello is the first thing on the wire', () => {
  it('greets the OSF UI runtime with osfui.hello as the helper loads, before any view code runs', () => {
    const { sent } = loadHelper();

    // The one boot path for first open, F5, hot-reload and crash recovery. The
    // OSF UI runtime answers `ready`, replays state, then opens this view's event gate
    // (MessageBridge::HandleHello) — nothing here has to be re-requested later.
    expect(sent).toEqual([{ kind: 'send', name: 'osfui.hello', payload: {} }]);
  });

  it('sends the greeting before the view can enqueue anything of its own', () => {
    const { helper, sent } = loadHelper();
    helper.send('close');

    expect(sent.map((f) => f.name)).toEqual(['osfui.hello', 'close']);
  });

  it('posts NOTHING without a bridge, and reports itself unavailable', () => {
    const { helper, raw } = loadHelper({ bridge: false });

    // `available` is a property in 2.0, not a call.
    expect(helper.available).toBe(false);
    expect(helper.send('close')).toBe(false);
    expect(raw).toEqual([]);
  });
});

describe('send envelopes', () => {
  it('are { kind:"send", name, payload } and never carry an id', () => {
    const { helper, sent } = loadHelper();

    expect(helper.send('close')).toBe(true);
    expect(sent[1]).toEqual({ kind: 'send', name: 'close', payload: {} });
    // An id on a send is a hard `invalid-request` at the OSF UI runtime: a caller that
    // supplied one expects a settlement that is never coming, and answering it
    // would resurrect the 1.x auto-ack.
    expect('id' in sent[1]!).toBe(false);
  });

  it('defaults a missing or falsy payload to {} rather than omitting it', () => {
    const { helper, sent } = loadHelper();

    helper.send('close');
    helper.send('log', undefined);
    helper.send('log', null as unknown as Record<string, unknown>);
    helper.send('log', 0 as unknown as Record<string, unknown>);

    // `payload || {}`. The OSF UI runtime rejects a present-but-non-object payload, so
    // the coercion is what keeps a sloppy call routable.
    for (const frame of sent.slice(1)) expect(frame.payload).toEqual({});
  });

  it('keeps routing metadata BESIDE the payload — a payload key cannot steer it', () => {
    const { helper, sent } = loadHelper();

    helper.send('close', { kind: 'request', name: 'evil', id: 'q9', command: 'evil' });

    // The 1.x wire put `command` inside the payload; 2.0's whole point is that
    // it cannot be spoofed from there.
    expect(sent[1]).toEqual({
      kind: 'send',
      name: 'close',
      payload: { kind: 'request', name: 'evil', id: 'q9', command: 'evil' },
    });
  });

  it('coerces the name to a string', () => {
    const { helper, sent } = loadHelper();

    helper.send(42 as unknown as string);

    // `String(name)`: the OSF UI runtime reads `name` as a string and treats a non-string
    // as an empty name, i.e. `invalid-request`. The coercion keeps a sloppy
    // caller routable instead of silently unroutable.
    expect(sent[1]!.name).toBe('42');
  });

  it('markReady() posts view.ready as a plain send', () => {
    const { helper, sent } = loadHelper();

    expect(helper.markReady()).toBe(true);
    expect(sent[1]).toEqual({ kind: 'send', name: 'view.ready', payload: {} });
    // Readiness is a notification, not a question: the reveal gate is the
    // OSF UI runtime's business (manifest `readySignal:true`), so there is nothing to
    // correlate.
    expect('id' in sent[1]!).toBe(false);
  });

  it('papyrus.send() folds its varargs into one fixed endpoint', () => {
    const { helper, sent } = loadHelper();

    helper.papyrus.send('OnThing', 1, 'two', true);

    // Sugar over a fixed endpoint, never a new wire shape: the Papyrus event
    // name travels in the payload, so `name` stays a platform endpoint.
    expect(sent[1]).toEqual({
      kind: 'send',
      name: 'papyrus.send',
      payload: { name: 'OnThing', args: [1, 'two', true] },
    });
  });

  it('papyrus.call() names a GLOBAL target and preserves scalar arguments', () => {
    const { helper, sent } = loadHelper();

    helper.papyrus.call('Acme:Widgets', 'SetEnabled', true, 3, 1.5,
      helper.papyrus.float(4), 'panel');

    expect(sent[1]).toEqual({
      kind: 'send',
      name: 'papyrus.call',
      payload: {
        script: 'Acme:Widgets',
        function: 'SetEnabled',
        args: [true, 3, 1.5, { $papyrus: 'float', value: 4 }, 'panel'],
      },
    });
  });
});

describe('request envelopes', () => {
  it('are { kind:"request", name, id, payload }', () => {
    const { helper, sent } = loadHelper();

    void helper.request('settings.set', { mod: 'm', key: 'k', value: 2 }).catch(() => {});

    expect(sent[1]).toEqual({
      kind: 'request',
      name: 'settings.set',
      id: 'q1',
      payload: { mod: 'm', key: 'k', value: 2 },
    });
  });

  it('generates "q" + a monotonic counter starting at 1', () => {
    const { helper, sent } = loadHelper();
    void helper.request('ping').catch(() => {});
    void helper.request('game.get').catch(() => {});

    // The greeting is a send and consumes no id, so the first request is q1.
    expect(sent[1]!.id).toBe('q1');
    expect(sent[2]!.id).toBe('q2');
  });

  it('gives two calls to the SAME endpoint distinct ids', () => {
    const { helper, sent } = loadHelper();
    void helper.request('ping').catch(() => {});
    void helper.request('ping').catch(() => {});

    // Correlation is by id alone; identical names must not collide.
    expect(sent[1]!.id).not.toBe(sent[2]!.id);
  });

  it('defaults a missing payload to {}', () => {
    const { helper, sent } = loadHelper();
    void helper.request('ping').catch(() => {});
    expect(sent[1]!.payload).toEqual({});
  });

  it('papyrus.request() folds its varargs into one fixed endpoint', () => {
    const { helper, sent } = loadHelper();

    void helper.papyrus.request('GetWeight', 0x14).catch(() => {});

    expect(sent[1]).toEqual({
      kind: 'request',
      name: 'papyrus.request',
      id: 'q1',
      payload: { name: 'GetWeight', args: [0x14] },
    });
  });
});

// ---------------------------------------------------------------------------
// Mirror of MessageBridge::HandleWebMessage's envelope gates
// (src/runtime/MessageBridge.cpp). Only ENVELOPE validity: an unknown endpoint
// name is a well-formed message that answers `unknown-endpoint`, not this.
// ---------------------------------------------------------------------------

const K_MAX_REQUEST_ID_LENGTH = 64;

type Verdict = 'ok' | 'invalid-request';

function bridgeVerdict(msg: Record<string, unknown>): Verdict {
  const kind = typeof msg.kind === 'string' ? msg.kind : '';
  if (kind !== 'send' && kind !== 'request') return 'invalid-request';

  const name = typeof msg.name === 'string' ? msg.name : '';
  if (name.length === 0) return 'invalid-request';

  // A present-but-non-object payload is a client bug, not something to coerce
  // (`it->is_object()`; an array is not an object to nlohmann either).
  if ('payload' in msg && msg.payload !== null) {
    if (typeof msg.payload !== 'object' || Array.isArray(msg.payload)) return 'invalid-request';
  }

  const hasId = 'id' in msg && msg.id !== null;
  if (kind === 'send') return hasId ? 'invalid-request' : 'ok';

  if (!hasId || typeof msg.id !== 'string') return 'invalid-request';
  if (msg.id.length === 0 || msg.id.length > K_MAX_REQUEST_ID_LENGTH) return 'invalid-request';
  return 'ok';
}

describe('OSF UI runtime envelope validation — 2.0 REJECTS where 1.x silently demoted', () => {
  it('accepts a request id of exactly 64 characters', () => {
    expect(bridgeVerdict({ kind: 'request', name: 'ping', id: 'q'.repeat(64) })).toBe('ok');
  });

  it('rejects a 65-char id as invalid-request instead of ignoring the id', () => {
    // 1.x treated an over-long id as ABSENT: the request was demoted to
    // fire-and-forget and the caller hung to its client timeout. 2.0 answers an
    // error the caller can settle on — a client bug that reports itself.
    expect(bridgeVerdict({ kind: 'request', name: 'ping', id: 'q'.repeat(65) })).toBe(
      'invalid-request',
    );
  });

  it('rejects a request with a missing, empty or non-string id', () => {
    expect(bridgeVerdict({ kind: 'request', name: 'ping' })).toBe('invalid-request');
    expect(bridgeVerdict({ kind: 'request', name: 'ping', id: '' })).toBe('invalid-request');
    expect(bridgeVerdict({ kind: 'request', name: 'ping', id: 7 })).toBe('invalid-request');
  });

  it('rejects an id on a SEND', () => {
    expect(bridgeVerdict({ kind: 'send', name: 'close', id: 'q1' })).toBe('invalid-request');
  });

  it('rejects an unknown kind, an empty name and a non-object payload', () => {
    expect(bridgeVerdict({ kind: 'ui.command', name: 'close' })).toBe('invalid-request');
    expect(bridgeVerdict({ kind: 'send', name: '' })).toBe('invalid-request');
    expect(bridgeVerdict({ kind: 'send', name: 'log', payload: 'text' })).toBe('invalid-request');
    expect(bridgeVerdict({ kind: 'send', name: 'log', payload: [1, 2] })).toBe('invalid-request');
  });

  it('accepts an absent or null payload', () => {
    expect(bridgeVerdict({ kind: 'send', name: 'close' })).toBe('ok');
    expect(bridgeVerdict({ kind: 'send', name: 'close', payload: null })).toBe('ok');
  });

  it('accepts EVERY envelope the shipped helper can produce', () => {
    const { helper, sent } = loadHelper();

    helper.send('close');
    helper.send('log', { level: 'info', message: 'hi' });
    helper.markReady();
    helper.papyrus.call('AcmeWidgets', 'Refresh');
    helper.papyrus.send('OnThing', 1);
    void helper.request('ping').catch(() => {});
    void helper.request('settings.set', { mod: 'm', key: 'k', value: 1 }).catch(() => {});
    void helper.papyrus.request('GetWeight', 0x14).catch(() => {});

    expect(sent.length).toBe(9); // hello + the eight above
    for (const frame of sent) {
      expect([frame.name, bridgeVerdict(frame as unknown as Record<string, unknown>)]).toEqual([
        frame.name,
        'ok',
      ]);
    }
  });

  it('keeps the helper own ids far inside the id limit', () => {
    const { helper, sent } = loadHelper();
    void helper.request('ping').catch(() => {});
    const id = sent[1]!.id!;

    expect(id.length).toBeLessThanOrEqual(K_MAX_REQUEST_ID_LENGTH);
    // "q" + counter stays short for any plausible session: a billion requests
    // is 11 characters.
    expect(('q' + 1e9).length).toBeLessThan(K_MAX_REQUEST_ID_LENGTH);
  });
});
