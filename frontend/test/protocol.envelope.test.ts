// @vitest-environment jsdom

import { describe, it, expect } from 'vitest';
import { readFileSync } from 'node:fs';
import { resolve } from 'node:path';

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
  papyrus: {
    float(value: number): { $papyrus: 'float'; value: number };
    call(script: string, fn: string, ...args: unknown[]): boolean;
  };
}

function loadHelper(opts?: { bridge?: boolean }): { helper: Helper; raw: string[]; sent: Frame[] } {
  const raw: string[] = [];
  const sent: Frame[] = [];
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
    expect('id' in sent[1]!).toBe(false);
  });

  it('defaults a missing or falsy payload to {} rather than omitting it', () => {
    const { helper, sent } = loadHelper();

    helper.send('close');
    helper.send('log', undefined);
    helper.send('log', null as unknown as Record<string, unknown>);
    helper.send('log', 0 as unknown as Record<string, unknown>);

    for (const frame of sent.slice(1)) expect(frame.payload).toEqual({});
  });

  it('keeps routing metadata BESIDE the payload — a payload key cannot steer it', () => {
    const { helper, sent } = loadHelper();

    helper.send('close', { kind: 'request', name: 'evil', id: 'q9', command: 'evil' });

    expect(sent[1]).toEqual({
      kind: 'send',
      name: 'close',
      payload: { kind: 'request', name: 'evil', id: 'q9', command: 'evil' },
    });
  });

  it('coerces the name to a string', () => {
    const { helper, sent } = loadHelper();

    helper.send(42 as unknown as string);

    expect(sent[1]!.name).toBe('42');
  });

  it('sends local Papyrus arguments through the generic endpoint envelope', () => {
    const { helper, sent } = loadHelper();

    helper.send('OnThing', { args: [1, 'two', true] });

    expect(sent[1]).toEqual({
      kind: 'send',
      name: 'OnThing',
      payload: { args: [1, 'two', true] },
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

    // The greeting is a send and consumes no id, so the first request is q1.
    expect(sent[1]!.id).toBe('q1');
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

  it('requests a local Papyrus endpoint through the generic endpoint envelope', () => {
    const { helper, sent } = loadHelper();

    void helper.request('GetWeight', { args: [0x14] }).catch(() => {});

    expect(sent[1]).toEqual({
      kind: 'request',
      name: 'GetWeight',
      id: 'q1',
      payload: { args: [0x14] },
    });
  });
});


const K_MAX_REQUEST_ID_LENGTH = 64;

type Verdict = 'ok' | 'invalid-request';

function bridgeVerdict(msg: Record<string, unknown>): Verdict {
  const kind = typeof msg.kind === 'string' ? msg.kind : '';
  if (kind !== 'send' && kind !== 'request') return 'invalid-request';

  const name = typeof msg.name === 'string' ? msg.name : '';
  if (name.length === 0) return 'invalid-request';

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
    helper.papyrus.call('AcmeWidgets', 'Refresh');
    helper.send('OnThing', { args: [1] });
    void helper.request('ping').catch(() => {});
    void helper.request('settings.set', { mod: 'm', key: 'k', value: 1 }).catch(() => {});
    void helper.request('GetWeight', { args: [0x14] }).catch(() => {});

    expect(sent.length).toBe(8); // hello + the seven above
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
    expect(('q' + 1e9).length).toBeLessThan(K_MAX_REQUEST_ID_LENGTH);
  });
});
