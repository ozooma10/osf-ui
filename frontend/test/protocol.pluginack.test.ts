// @vitest-environment jsdom
//
// Settling rules for qualified plugin requests ("<author>.<modname>.<name>",
// three segments minimum) in the shipped helper (src/shared-kit/osfui.js).
//
// The host sends the RegisterCommand auto-ack `ui.result { ok:true, command }`
// ONLY when the handler replied with nothing at all — any requestId-echoing
// reply suppresses it (MessageBridge::SendResult / SendToWeb set _replied).
// An ack that names the request's own command is therefore the host's
// definitive "nothing else is coming", and the helper must settle on it.
// Before this rule, the documented minimum handler path (RegisterCommand with
// no reply of its own — docs/native-plugin-api.md, "delivered, not succeeded")
// left every schema `action` button pending for the full ACTION_TIMEOUT_MS and
// then toasted a false "No response from {mod}".

import { describe, it, expect } from 'vitest';
import { readFileSync } from 'node:fs';
import { resolve } from 'node:path';

const HELPER_SRC = readFileSync(resolve(process.cwd(), 'src/shared-kit/osfui.js'), 'utf8');

interface Frame {
  type: string;
  requestId?: string;
  payload: Record<string, unknown>;
}

interface Helper {
  request(
    command: string,
    fields?: Record<string, unknown>,
    opts?: { timeoutMs?: number },
  ): Promise<{ type: string; payload: Record<string, unknown> }>;
  onMessage(json: string): void;
}

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

/** Settled state probe that never awaits the timeout. */
function probe<T>(p: Promise<T>): { settled(): boolean; value(): T } {
  let settled = false;
  let value: T;
  p.then(
    (v) => {
      settled = true;
      value = v;
    },
    () => {
      settled = true;
    },
  );
  return {
    settled: () => settled,
    value: () => value,
  };
}

const tick = () => new Promise<void>((r) => setTimeout(r, 0));

describe('plugin request — auto-ack settling', () => {
  it('resolves a plugin request on the ack that names its own command', async () => {
    const { helper, sent } = loadHelper();
    const p = probe(helper.request('acme.mymod.ping', { id: 'x' }, { timeoutMs: 0 }));
    const rid = sent[0]!.requestId!;

    helper.onMessage(
      JSON.stringify({
        type: 'ui.result',
        requestId: rid,
        payload: { ok: true, command: 'acme.mymod.ping' },
      }),
    );
    await tick();

    expect(p.settled()).toBe(true);
    expect(p.value().type).toBe('ui.result');
    expect(p.value().payload.ok).toBe(true);
  });

  it('keeps waiting on an ack for some OTHER command (defensive)', async () => {
    const { helper, sent } = loadHelper();
    const p = probe(helper.request('acme.mymod.ping', undefined, { timeoutMs: 0 }));
    const rid = sent[0]!.requestId!;

    helper.onMessage(
      JSON.stringify({
        type: 'ui.result',
        requestId: rid,
        payload: { ok: true, command: 'acme.mymod.other' },
      }),
    );
    await tick();
    expect(p.settled()).toBe(false);

    // The real typed reply still settles afterwards.
    helper.onMessage(
      JSON.stringify({
        type: 'acme.mymod.pong',
        requestId: rid,
        payload: { value: 7 },
      }),
    );
    await tick();
    expect(p.settled()).toBe(true);
    expect(p.value().type).toBe('acme.mymod.pong');
  });

  it('still resolves a plugin request on a typed reply (RegisterRequest path)', async () => {
    const { helper, sent } = loadHelper();
    const p = probe(helper.request('acme.mymod.getWeight', { formId: 1 }, { timeoutMs: 0 }));
    const rid = sent[0]!.requestId!;

    helper.onMessage(
      JSON.stringify({
        type: 'acme.mymod.weight',
        requestId: rid,
        payload: { weight: 12.5 },
      }),
    );
    await tick();

    expect(p.settled()).toBe(true);
    expect(p.value().type).toBe('acme.mymod.weight');
    expect(p.value().payload.weight).toBe(12.5);
  });

  it('still rejects a plugin request on ui.result ok:false', async () => {
    const { helper, sent } = loadHelper();
    let code = '';
    const p = helper
      .request('acme.mymod.ping', undefined, { timeoutMs: 0 })
      .catch((e: { code?: string }) => {
        code = e.code ?? '';
      });
    const rid = sent[0]!.requestId!;

    helper.onMessage(
      JSON.stringify({
        type: 'ui.result',
        requestId: rid,
        payload: { ok: false, command: 'acme.mymod.ping', code: 'invalid-payload' },
      }),
    );
    await p;
    expect(code).toBe('invalid-payload');
  });

  it('platform (non-plugin) requests settle on any correlated ui.result, as before', async () => {
    const { helper, sent } = loadHelper();
    const p = probe(helper.request('settings.set', { mod: 'demo' }, { timeoutMs: 0 }));
    const rid = sent[0]!.requestId!;

    helper.onMessage(
      JSON.stringify({
        type: 'ui.result',
        requestId: rid,
        payload: { ok: true, command: 'settings.set' },
      }),
    );
    await tick();
    expect(p.settled()).toBe(true);
  });
});
