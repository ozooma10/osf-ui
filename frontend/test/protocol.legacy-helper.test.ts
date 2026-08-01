// @vitest-environment jsdom

import { describe, it, expect, afterEach } from 'vitest';
import { readFileSync } from 'node:fs';
import { resolve } from 'node:path';

const HELPER_SRC = readFileSync(resolve(process.cwd(), 'src/shared-kit/osfui.js'), 'utf8');

interface LegacyHelper {
  available(): boolean;
  ready: Promise<Record<string, unknown>>;
  send(name: string, payload?: Record<string, unknown>): boolean;
  emit(name: string, payload?: Record<string, unknown>): boolean;
  action(name: string, ...args: unknown[]): boolean;
  request(name: string, payload?: Record<string, unknown>): Promise<{ type: string; payload: unknown }>;
  call(name: string, payload?: Record<string, unknown>): Promise<unknown>;
  viewReady(): boolean;
  on(name: string, fn: (payload: unknown) => void): () => void;
  onMessage(json: string): void;
}

function loadLegacyHelper(): { helper: LegacyHelper; sent: Array<Record<string, unknown>> } {
  window.history.replaceState({}, '', '/starcade.arcade/launcher/index.html?osfui-api=1');
  const sent: Array<Record<string, unknown>> = [];
  (window as unknown as { osfui: unknown }).osfui = {
    postMessage(json: string) { sent.push(JSON.parse(json) as Record<string, unknown>); },
  };
  new Function(HELPER_SRC)();
  return { helper: window.osfui as unknown as LegacyHelper, sent };
}

afterEach(() => {
  window.history.replaceState({}, '', '/');
  delete (window as unknown as { osfui?: unknown }).osfui;
});

describe('pre-2.0 manifest helper compatibility', () => {
  it('preserves the Starcade helper surface over 2.0 envelopes', async () => {
    const { helper, sent } = loadLegacyHelper();

    expect(helper.available()).toBe(true);
    expect(helper.send('starcade.arcade.score.submit', { game: 'slots', score: 10 })).toBe(true);
    expect(helper.emit('osfui.gamepadRaw', { raw: true })).toBe(true);
    expect(helper.viewReady()).toBe(true);
    expect(sent).toEqual([
      { kind: 'send', name: 'osfui.hello', payload: {} },
      { kind: 'send', name: 'starcade.arcade.score.submit', payload: { game: 'slots', score: 10 } },
      { kind: 'send', name: 'osfui.gamepadRaw', payload: { raw: true } },
      { kind: 'send', name: 'view.ready', payload: {} },
    ]);

    helper.onMessage(JSON.stringify({ kind: 'ready', payload: { mod: 'starcade.arcade' } }));
    await expect(helper.ready).resolves.toMatchObject({ mod: 'starcade.arcade' });
  });

  it('maps the 1.x action helper to the 2.0 Papyrus send endpoint', () => {
    const { helper, sent } = loadLegacyHelper();
    helper.action('ready', 'launcher');

    expect(sent[1]).toEqual({
      kind: 'send',
      name: 'papyrus.send',
      payload: { name: 'ready', args: ['launcher'] },
    });
  });

  it('keeps the 1.x request envelope and call payload shapes', async () => {
    const { helper, sent } = loadLegacyHelper();
    const requested = helper.request('starcade.arcade.query');
    const requestId = String(sent[1]?.id);
    helper.onMessage(JSON.stringify({ kind: 'reply', id: requestId, payload: { score: 7 } }));
    await expect(requested).resolves.toEqual({ type: 'ui.result', payload: { score: 7 } });

    const called = helper.call('starcade.arcade.query');
    const callId = String(sent[2]?.id);
    helper.onMessage(JSON.stringify({ kind: 'reply', id: callId, payload: { score: 8 } }));
    await expect(called).resolves.toEqual({ score: 8 });
  });

  it('keeps mod-defined events on the established on() surface', () => {
    const { helper } = loadLegacyHelper();
    let state: unknown;
    helper.on('starcade.state', (payload) => { state = payload; });
    helper.onMessage(JSON.stringify({ kind: 'event', name: 'starcade.state', payload: { credits: 42 } }));

    expect(state).toEqual({ credits: 42 });
  });
});
