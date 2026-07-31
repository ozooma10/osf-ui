// @vitest-environment jsdom
//
// Outbound envelope shape: what the shipped helper (src/shared-kit/osfui.js,
// frozen bridge protocol 1.0) emits and what src/runtime/MessageBridge.cpp
// parses. Every oddity asserted here is load-bearing. jsdom is needed because
// half the file drives the real helper (an IIFE decorating `window.osfui`).

import { describe, it, expect } from 'vitest';
import { readFileSync } from 'node:fs';
import { resolve } from 'node:path';


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
    expect(nativeExtractRequestId({ requestId: id })).toBe(id);
  });

  it('DROPS a 65-char id rather than truncating it', () => {
    const id = 'q'.repeat(65);
    const env = { requestId: id };

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
