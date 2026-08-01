// @vitest-environment jsdom
//
// Settling rules for mod-backend requests ("<author>.<modname>.<name>", three
// segments minimum) in the shipped helper (src/shared-kit/osfui.js).
//
// This file used to pin an auto-ack HEURISTIC. In 1.x a RegisterCommand handler
// that replied with nothing produced a host `ui.result { ok:true, command }`,
// and the helper had to guess whether an ack that named the request's own
// command meant "nothing else is coming" — otherwise every schema `action`
// button hung for the full timeout and toasted a false "No response from {mod}".
//
// 2.0 deletes the helper's guess. Kind decides strict endpoint behavior:
//   - `send()` is never awaited and needs no ack; it returns as soon as the
//     message is posted. Native ABI RegisterCommand keeps a host-side 1.x
//     compatibility auto-ack when explicitly reached through request().
//   - a request endpoint MUST settle exactly once (Respond / Reject / Defer),
//     and the host answers `internal` if a handler returns without settling
//     (MessageBridge::DispatchRequest), so "no answer" is a bug that reports
//     itself instead of a hang.
//   - correlation is by `id` alone. Nothing in a payload steers it, and a
//     settlement never reaches on() handlers the way a 1.x reply did.

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
    opts?: { timeoutMs?: number },
  ): Promise<unknown>;
  on(event: string, fn: (payload: unknown) => void): () => void;
  papyrus: { request(name: string, ...args: unknown[]): Promise<unknown> };
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
  // sent[0] is the helper's own `osfui.hello`; a request's id is sent[1].id.
  return { helper: window.osfui as unknown as Helper, sent };
}

function deliver(helper: Helper, message: unknown): void {
  helper.onMessage(JSON.stringify(message));
}

/** Settled-state probe that never awaits the timeout and never leaks a rejection. */
function probe<T>(p: Promise<T>): {
  settled(): boolean;
  value(): T | undefined;
  error(): { code?: unknown } | undefined;
} {
  let settled = false;
  let value: T | undefined;
  let error: { code?: unknown } | undefined;
  p.then(
    (v) => {
      settled = true;
      value = v;
    },
    (e: { code?: unknown }) => {
      settled = true;
      error = e;
    },
  );
  return { settled: () => settled, value: () => value, error: () => error };
}

/** Drain the microtask queue so a settled promise's handlers have run. */
async function flush(): Promise<void> {
  await Promise.resolve();
  await Promise.resolve();
}

beforeEach(() => {
  // Fake timers so the default 10 s client timer never fires mid-case; the
  // timer itself is pinned in protocol.errors.test.ts.
  vi.useFakeTimers();
  vi.spyOn(console, 'error').mockImplementation(() => {});
});

afterEach(() => {
  vi.useRealTimers();
  vi.restoreAllMocks();
});

describe('a settlement is not an event', () => {
  it('never reaches an on() subscriber, however it is named', async () => {
    const { helper, sent } = loadHelper();
    const seen: unknown[] = [];
    // 1.x fanned a reply out to subscribers as well, so one render path could
    // consume "settings.data" no matter who asked. 2.0 replies carry no name at
    // all — there is nothing for a subscriber to match.
    helper.on('acme.mymod.weight', (p) => seen.push(p));
    helper.on('reply', (p) => seen.push(p));
    helper.on('', (p) => seen.push(p));

    const p = probe(helper.request('acme.mymod.getWeight', { formId: 0x14 }));
    deliver(helper, { kind: 'reply', id: sent[1]!.id!, payload: { weight: 12.5 } });
    await flush();

    expect(p.value()).toEqual({ weight: 12.5 });
    expect(seen).toEqual([]);
  });

  it('never reaches an on() subscriber when it is a rejection either', async () => {
    const { helper, sent } = loadHelper();
    const seen: unknown[] = [];
    helper.on('error', (p) => seen.push(p));
    helper.on('ui.error', (p) => seen.push(p));

    const p = probe(helper.request('acme.mymod.ping'));
    deliver(helper, {
      kind: 'error',
      id: sent[1]!.id!,
      payload: { code: 'invalid-payload', message: 'formId is required' },
    });
    await flush();

    expect(p.error()!.code).toBe('invalid-payload');
    expect(seen).toEqual([]);
  });

  it('and an event never settles a request, even one carrying an id', async () => {
    const { helper, sent } = loadHelper();
    const seen: unknown[] = [];
    helper.on('acme.mymod.weight', (p) => seen.push(p));

    const p = probe(helper.request('acme.mymod.getWeight'));
    const id = sent[1]!.id!;

    // A backend that emits an event instead of answering has a bug the host
    // catches (`internal` at the end of DispatchRequest). The helper must not
    // paper over it by settling on the event.
    deliver(helper, { kind: 'event', name: 'acme.mymod.weight', id, payload: { weight: 12.5 } });
    await flush();

    expect(seen).toEqual([{ weight: 12.5 }]);
    expect(p.settled()).toBe(false);
  });
});

describe('a request settles exactly once', () => {
  it('resolves on the first reply and ignores a duplicate', async () => {
    const { helper, sent } = loadHelper();
    const p = probe(helper.request('acme.mymod.getWeight'));
    const id = sent[1]!.id!;

    deliver(helper, { kind: 'reply', id, payload: { weight: 12.5 } });
    deliver(helper, { kind: 'reply', id, payload: { weight: 99 } });
    await flush();

    // The pending entry is deleted on the first settle, so a late or duplicate
    // frame is inert — it cannot overwrite the value or raise an unhandled
    // rejection in a page that has already moved on.
    expect(p.value()).toEqual({ weight: 12.5 });
  });

  it('stays resolved when an error arrives after the reply', async () => {
    const { helper, sent } = loadHelper();
    const p = probe(helper.request('acme.mymod.ping'));
    const id = sent[1]!.id!;

    deliver(helper, { kind: 'reply', id, payload: { ok: true } });
    deliver(helper, { kind: 'error', id, payload: { code: 'too-late' } });
    await flush();

    expect(p.value()).toEqual({ ok: true });
    expect(p.error()).toBeUndefined();
  });

  it('stays rejected when a reply arrives after the error', async () => {
    const { helper, sent } = loadHelper();
    const p = probe(helper.request('acme.mymod.ping'));
    const id = sent[1]!.id!;

    deliver(helper, { kind: 'error', id, payload: { code: 'invalid-payload' } });
    deliver(helper, { kind: 'reply', id, payload: { ok: true } });
    await flush();

    expect(p.error()!.code).toBe('invalid-payload');
    expect(p.value()).toBeUndefined();
  });

  it('settles nothing on an id that matches no request', async () => {
    const { helper, sent } = loadHelper();
    const p = probe(helper.request('acme.mymod.getWeight'));
    const id = sent[1]!.id!;

    // A stale id from the previous document, or a host echoing something this
    // page never asked for.
    expect(() => deliver(helper, { kind: 'reply', id: 'q999', payload: { weight: 1 } })).not.toThrow();
    await flush();
    expect(p.settled()).toBe(false);

    // The real reply still works afterwards; the stray frame consumed nothing.
    deliver(helper, { kind: 'reply', id, payload: { weight: 12.5 } });
    await flush();
    expect(p.value()).toEqual({ weight: 12.5 });
  });

  it('settles nothing on a missing or empty id', async () => {
    const { helper } = loadHelper();
    const p = probe(helper.request('acme.mymod.getWeight'));

    expect(() => deliver(helper, { kind: 'reply', payload: { weight: 1 } })).not.toThrow();
    expect(() => deliver(helper, { kind: 'error', id: '', payload: { code: 'x' } })).not.toThrow();
    await flush();

    expect(p.settled()).toBe(false);
  });

  it('keeps concurrent requests independent, settling them by id in any order', async () => {
    const { helper, sent } = loadHelper();
    const first = probe(helper.request('acme.mymod.getWeight', { formId: 1 }));
    const second = probe(helper.request('acme.mymod.getWeight', { formId: 2 }));
    const [firstId, secondId] = [sent[1]!.id!, sent[2]!.id!];

    deliver(helper, { kind: 'reply', id: secondId, payload: { weight: 2 } });
    await flush();
    expect(second.value()).toEqual({ weight: 2 });
    expect(first.settled()).toBe(false);

    deliver(helper, { kind: 'reply', id: firstId, payload: { weight: 1 } });
    await flush();
    expect(first.value()).toEqual({ weight: 1 });
  });
});

describe('correlation is by id alone — no payload heuristic survives', () => {
  it('resolves on a reply whose payload names a DIFFERENT endpoint', async () => {
    const { helper, sent } = loadHelper();
    const p = probe(helper.request('acme.mymod.ping'));

    // 1.x read `payload.command` to decide whether an ack was "mine". Nothing
    // reads it now, so a backend that echoes the wrong name (or none) cannot
    // strand its caller.
    deliver(helper, {
      kind: 'reply',
      id: sent[1]!.id!,
      payload: { ok: true, command: 'acme.mymod.somethingElse' },
    });
    await flush();

    expect(p.value()).toEqual({ ok: true, command: 'acme.mymod.somethingElse' });
  });

  it('treats a platform request exactly like a mod request', async () => {
    const { helper, sent } = loadHelper();
    const p = probe(helper.request('settings.set', { mod: 'demo', key: 'k', value: 1 }));

    // There is no plugin/platform split in the settling rules any more — the
    // three-segment name grammar only keeps the two namespaces collision-proof.
    deliver(helper, {
      kind: 'reply',
      id: sent[1]!.id!,
      payload: { mod: 'demo', key: 'k', value: 1 },
    });
    await flush();

    expect(p.value()).toEqual({ mod: 'demo', key: 'k', value: 1 });
  });

  it('unwraps papyrus.request to the reply payload value', async () => {
    const { helper, sent } = loadHelper();
    const p = probe(helper.papyrus.request('GetWeight', 0x14));

    deliver(helper, { kind: 'reply', id: sent[1]!.id!, payload: { value: 12.5 } });
    await flush();

    // The one endpoint whose resolution is not the raw payload: the Papyrus
    // listener's return value is what the author asked for.
    expect(p.value()).toBe(12.5);
  });
});

describe('send() is one-way — nothing to await, nothing to ack', () => {
  it('returns immediately and schedules no timer', () => {
    const { helper, sent } = loadHelper();

    // The case that motivated the deleted heuristic: a mod action button whose
    // backend only needs to be told. It is a send now, so there is no pending
    // entry, no client timer, and no false "No response from {mod}" toast to
    // suppress.
    expect(helper.send('acme.mymod.doThing', { id: 'x' })).toBe(true);
    expect(sent[1]).toEqual({ kind: 'send', name: 'acme.mymod.doThing', payload: { id: 'x' } });
    expect(vi.getTimerCount()).toBe(0);
  });

  it('rejects with "internal" when a request endpoint answers nothing', async () => {
    const { helper, sent } = loadHelper();
    const p = probe(helper.request('acme.mymod.getWeight'));

    // The host's backstop for the same authoring mistake on the request side
    // (DispatchRequest: "the endpoint did not answer"). The page learns it as an
    // ordinary typed rejection rather than waiting out a timeout.
    deliver(helper, {
      kind: 'error',
      id: sent[1]!.id!,
      payload: { code: 'internal', message: 'the endpoint did not answer' },
    });
    await flush();

    expect(p.error()!.code).toBe('internal');
  });
});
