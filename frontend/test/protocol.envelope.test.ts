// @vitest-environment jsdom
//
// Outbound envelope shape: what the shipped helper (src/shared-kit/osfui.js,
// frozen bridge protocol 1.0) emits and what src/runtime/MessageBridge.cpp
// parses. Every oddity asserted here is load-bearing. jsdom is needed because
// half the file drives the real helper (an IIFE decorating `window.osfui`).

import { describe, it, expect } from 'vitest';
import { readFileSync } from 'node:fs';
import { resolve } from 'node:path';
import { encodeCommand } from '@lib/protocol';

// Read from disk, not imported: osfui.js is a classic script. Resolved against
// the vitest root (frontend/) because under jsdom `import.meta.url` is an http:
// URL, not a file: one.
const HELPER_SRC = readFileSync(resolve(process.cwd(), 'src/shared-kit/osfui.js'), 'utf8');

interface Frame {
  type: string;
  requestId?: string;
  payload: Record<string, unknown>;
}

interface Helper {
  available(): boolean;
  send(command: string, fields?: Record<string, unknown>): boolean;
  viewReady(): boolean;
  request(
    command: string,
    fields?: Record<string, unknown>,
    opts?: { timeoutMs?: number },
  ): Promise<unknown>;
  onMessage(json: string): void;
}

/**
 * Install a fake native bridge and evaluate the shipped helper over it.
 *
 * `new Function` rather than import: osfui.js is a classic script whose
 * `window` / `document` / `setTimeout` references must bind to jsdom's globals.
 * A fresh `window.osfui` per call resets the helper's private `seq` closure, so
 * request ids are deterministic per test.
 */
function loadHelper(): { helper: Helper; raw: string[]; sent: Frame[] } {
  const raw: string[] = [];
  const sent: Frame[] = [];
  (window as unknown as { osfui: unknown }).osfui = {
    postMessage(json: string) {
      raw.push(json);
      sent.push(JSON.parse(json) as Frame);
    },
  };
  new Function(HELPER_SRC)();
  return { helper: window.osfui as unknown as Helper, raw, sent };
}

describe('encodeCommand — outbound envelope shape', () => {
  it('nests the command name INSIDE the payload', () => {
    const env = encodeCommand('settings.set', { mod: 'demo', key: 'x', value: 1 });

    // Contract, not a bug: MessageBridge reads `payload.command`, and the
    // shipped helper builds the payload as `Object.assign({ command }, fields)`.
    // There is no top-level `command` key and native would not look at one.
    expect(env).toEqual({
      type: 'ui.command',
      payload: { command: 'settings.set', mod: 'demo', key: 'x', value: 1 },
    });
    expect(env).not.toHaveProperty('command');
    expect(env.payload.command).toBe('settings.set');
  });

  it('always uses type "ui.command" — the only web->native envelope type', () => {
    expect(encodeCommand('ping').type).toBe('ui.command');
  });

  it('emits a payload carrying only `command` when no fields are given', () => {
    expect(encodeCommand('ping').payload).toEqual({ command: 'ping' });
  });

  it('lets a caller-supplied `command` field WIN over the command argument', () => {
    // Object.assign({ command }, fields) copies fields last, so a stray
    // `command` in fields overwrites the argument. The shipped helper does the
    // same, and a mod action's field bag is caller-controlled; changing the
    // precedence would change which command native dispatches for existing views.
    const env = encodeCommand('demo.doThing', { command: 'close' });
    expect(env.payload.command).toBe('close');
  });

  it('omits requestId ENTIRELY when none is supplied', () => {
    const env = encodeCommand('close');

    // Absent, not present-and-undefined. Native treats an absent id as
    // fire-and-forget and sends no ui.result, so a stray key changes the reply
    // behaviour.
    expect('requestId' in env).toBe(false);
    expect(JSON.parse(JSON.stringify(env))).not.toHaveProperty('requestId');
  });

  it('attaches requestId when supplied, including the empty string', () => {
    expect(encodeCommand('ping', undefined, 'q7').requestId).toBe('q7');

    // Only `=== undefined` suppresses the key, so "" is attached — and native
    // then rejects the empty id, degrading the call to fire-and-forget.
    // Documented, not fixed.
    const empty = encodeCommand('ping', undefined, '');
    expect('requestId' in empty).toBe(true);
    expect(empty.requestId).toBe('');
  });

  it('matches the shipped helper frame semantically (key ORDER differs)', () => {
    const { helper, raw } = loadHelper();
    helper.send('log', { text: 'hi' });

    const shipped = raw[0]!;
    const ours = JSON.stringify(encodeCommand('log', { text: 'hi' }));
    expect(JSON.parse(ours)).toEqual(JSON.parse(shipped));
    // For a fire-and-forget frame the serialisations are identical.
    expect(ours).toBe(shipped);
  });

  it('serialises requestId LAST, unlike the shipped helper (comment is stale)', () => {
    const { helper, raw } = loadHelper();
    void helper.request('ping').catch(() => {});

    const shipped = raw[0]!;
    const ours = JSON.stringify(encodeCommand('ping', undefined, 'q1'));

    // protocol.ts claims the emitted JSON is byte-identical to the helper's. It
    // is not: the helper builds {type, requestId, payload} in one literal while
    // encodeCommand appends requestId after payload. Harmless — both sides parse
    // JSON — but assert the real bytes so nobody "verifies" the stale comment.
    expect(ours).not.toBe(shipped);
    expect(JSON.parse(ours)).toEqual(JSON.parse(shipped));
    expect(Object.keys(JSON.parse(ours) as object)).toEqual(['type', 'payload', 'requestId']);
    expect(Object.keys(JSON.parse(shipped) as object)).toEqual(['type', 'requestId', 'payload']);
  });
});

describe('shipped helper — request id format', () => {
  it('generates "q" + a monotonic counter starting at 1', () => {
    const { helper, sent } = loadHelper();
    void helper.request('ping').catch(() => {});
    void helper.request('game.get').catch(() => {});

    expect(sent[0]!.requestId).toBe('q1');
    expect(sent[1]!.requestId).toBe('q2');
  });

  it('never puts a requestId on send() — send is fire-and-forget by design', () => {
    const { helper, sent } = loadHelper();
    expect(helper.send('close')).toBe(true);
    expect('requestId' in sent[0]!).toBe(false);
  });

  it('viewReady() emits the protocol-1.2 readiness command without correlation', () => {
    const { helper, sent } = loadHelper();

    expect(helper.viewReady()).toBe(true);
    expect(sent).toEqual([{
      type: 'ui.command',
      payload: { command: 'view.ready' },
    }]);
    expect('requestId' in sent[0]!).toBe(false);
  });
});

// Mirror of ExtractRequestId in src/runtime/MessageBridge.cpp: an executable
// statement of the native rule the JS side must respect — ids are bounded, and
// an over-long one is dropped, never shortened.
const K_MAX_REQUEST_ID_LENGTH = 64;
function nativeExtractRequestId(msg: { requestId?: unknown }): string {
  if (typeof msg.requestId !== 'string') return '';
  if (msg.requestId.length === 0 || msg.requestId.length > K_MAX_REQUEST_ID_LENGTH) return '';
  return msg.requestId;
}

describe('requestId length — native treats an over-long id as ABSENT', () => {
  it('accepts an id of exactly 64 chars', () => {
    const id = 'q'.repeat(64);
    expect(nativeExtractRequestId(encodeCommand('ping', undefined, id))).toBe(id);
  });

  it('DROPS a 65-char id rather than truncating it', () => {
    const id = 'q'.repeat(65);
    const env = encodeCommand('ping', undefined, id);

    // The JS side sends it; native ignores it and answers nothing, so the
    // caller's request() hangs until its timeout. A truncated id would be worse
    // — it would never correlate — which is why native drops instead.
    expect(env.requestId).toBe(id);
    expect(nativeExtractRequestId(env)).toBe('');
    expect(nativeExtractRequestId(env)).not.toBe(id.slice(0, 64));
  });

  it('keeps the shipped helper own ids far inside the limit', () => {
    const { helper, sent } = loadHelper();
    void helper.request('ping').catch(() => {});
    const id = sent[0]!.requestId!;

    expect(id.length).toBeLessThanOrEqual(K_MAX_REQUEST_ID_LENGTH);
    expect(nativeExtractRequestId(sent[0]!)).toBe(id);
    // "q" + counter stays short for any plausible session: a billion requests
    // is 11 chars.
    expect(('q' + 1e9).length).toBeLessThan(K_MAX_REQUEST_ID_LENGTH);
  });
});
