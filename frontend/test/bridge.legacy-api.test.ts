// @vitest-environment jsdom

import { beforeEach, describe, expect, it, vi } from 'vitest';
import { composeHelper } from '../scripts/compose-helper.mjs';
import type { GameInputContextClassification, VanillaContextClassification } from '@sdk';

// Compile and exercise the deprecated SDK spelling only in this compatibility
// suite; current Keybindings tests use GameInputContextClassification.
const legacyClassification: VanillaContextClassification =
  'core' satisfies GameInputContextClassification;

describe('deprecated SDK aliases', () => {
  it('keeps VanillaContextClassification source-compatible', () => {
    expect(legacyClassification).toBe('core');
  });
});

interface Sent {
  kind: 'send' | 'request';
  name: string;
  id?: string;
  payload: Record<string, unknown>;
}

type LegacyHelper = {
  available(): boolean;
  ready: Promise<Record<string, unknown>>;
  send(name: string, payload?: Record<string, unknown>): boolean;
  emit(name: string, payload?: Record<string, unknown>): boolean;
  action(name: string, ...args: unknown[]): boolean;
  request(name: string, payload?: Record<string, unknown>, opts?: { timeoutMs?: number }): Promise<any>;
  call(name: string, payload?: Record<string, unknown>, opts?: { timeoutMs?: number }): Promise<any>;
  on(type: string, fn: (payload: any, message: any) => void): () => void;
  onMessage(json: string): void;
  data: { get(key: string): unknown; on(key: string, fn: (...args: any[]) => void): () => void };
  papyrus: { action(name: string, ...args: unknown[]): boolean; request(name: string, ...args: unknown[]): Promise<unknown> };
  i18nReady: Promise<unknown>;
  locale(): string;
  t(address: string, english: string, vars?: Record<string, unknown>): string;
  localize(root?: ParentNode): void;
  applyAccent(element: HTMLElement, hex: string): void;
};

function load(search: string): { helper: LegacyHelper; sent: Sent[] } {
  window.history.replaceState({}, '', '/acme.widgets/panel/index.html' + search);
  const sent: Sent[] = [];
  (window as any).osfui = {
    postMessage(json: string) { sent.push(JSON.parse(json) as Sent); },
  };
  new Function(composeHelper())();
  return { helper: (window as any).osfui as LegacyHelper, sent };
}

function deliver(helper: LegacyHelper, message: unknown): void {
  helper.onMessage(JSON.stringify(message));
}

function last(sent: Sent[]): Sent {
  return sent[sent.length - 1]!;
}

beforeEach(() => {
  document.documentElement.lang = '';
  document.body.replaceChildren();
});

describe('frozen 1.x helper facade', () => {
  it('is absent from a 2.0 navigation', () => {
    const { helper } = load('?scenario=strict');
    expect(typeof (helper as any).available).toBe('boolean');
    expect((helper as any).emit).toBeUndefined();
    expect((helper as any).action).toBeUndefined();
    expect((helper as any).call).toBeUndefined();
    expect((helper as any).data).toBeUndefined();
    expect((helper as any).i18nReady).toBeUndefined();
    expect((helper as any).locale).toBeUndefined();
    expect((helper as any).t).toBeUndefined();
    expect((helper as any).localize).toBeUndefined();
    expect((helper as any).applyAccent).toBeUndefined();
    expect((helper as any).papyrus.action).toBeUndefined();
  });

  it('restores aliases while preserving query and fragment selection', async () => {
    const { helper, sent } = load('?scenario=demo&osfui-api=1#inventory');
    expect(helper.available()).toBe(true);
    expect(helper.emit('acme.widgets.refresh', { now: true })).toBe(true);
    expect(last(sent)).toEqual({
      kind: 'send', name: 'acme.widgets.refresh', payload: { now: true },
    });
    expect(helper.action('sort', 'aid', 4)).toBe(true);
    expect(last(sent)).toEqual({
      kind: 'send', name: 'papyrus.send', payload: { name: 'sort', args: ['aid', 4] },
    });
    expect(helper.emit('ui.action', { action: 'equip', arg: 'weapon' })).toBe(true);
    expect(last(sent)).toEqual({
      kind: 'send', name: 'papyrus.send', payload: { name: 'equip', args: ['weapon'] },
    });
    expect(helper.papyrus.action('equip', 42)).toBe(true);
    expect(last(sent)).toEqual({
      kind: 'send', name: 'papyrus.send', payload: { name: 'equip', args: [42] },
    });
    const pong = vi.fn();
    helper.on('runtime.pong', pong);
    expect(helper.send('ping')).toBe(true);
    const ping = last(sent);
    expect(ping).toMatchObject({ kind: 'request', name: 'ping' });
    deliver(helper, { kind: 'reply', id: ping.id, payload: {} });
    await vi.waitFor(() => {
      expect(pong).toHaveBeenCalledWith(
        {}, { type: 'runtime.pong', requestId: 'q1', payload: {} },
      );
    });
  });

  it('translates aliases and reconstructs request envelopes with reply fan-out', async () => {
    const { helper, sent } = load('?osfui-api=1');
    const listener = vi.fn();
    helper.on('papyrus.result', listener);
    const result = helper.request('ui.papyrusRequest', { request: 'price', args: [42] });
    const outbound = last(sent);
    expect(outbound).toMatchObject({
      kind: 'request', name: 'papyrus.request', payload: { name: 'price', args: [42] },
    });
    deliver(helper, { kind: 'reply', id: outbound.id, payload: { value: 125 } });
    const message = await result;
    expect(message).toEqual({ type: 'papyrus.result', requestId: 'q1', payload: { value: 125 } });
    expect(listener).toHaveBeenCalledWith(message.payload, message);

    const call = helper.call('hud.show', { view: 'acme.widgets/panel' });
    const alias = last(sent);
    expect(alias).toMatchObject({ kind: 'request', name: 'menu.open' });
    deliver(helper, { kind: 'reply', id: alias.id, payload: { open: true } });
    await expect(call).resolves.toEqual({ open: true });

    const sendRequest = helper.request('log', { text: 'legacy request shape' });
    const sendOutbound = last(sent);
    expect(sendOutbound).toMatchObject({ kind: 'request', name: 'log' });
    deliver(helper, {
      kind: 'reply', id: sendOutbound.id, payload: { ok: true, command: 'log' },
    });
    await expect(sendRequest).resolves.toEqual({
      type: 'ui.result', requestId: 'q3', payload: { ok: true, command: 'log' },
    });

    const typed = helper.request('acme.widgets.lookup', { item: 7 });
    const typedOutbound = last(sent);
    deliver(helper, {
      kind: 'reply', id: typedOutbound.id,
      payload: { __osfuiV1Reply: { type: 'acme.widgets.result', payload: { found: true } } },
    });
    const typedMessage = await typed;
    expect(typedMessage).toEqual({
      type: 'acme.widgets.result', requestId: 'q4', payload: { found: true },
    });

    const papyrus = helper.papyrus.request('cost', 42);
    const papyrusOutbound = last(sent);
    expect(papyrusOutbound).toMatchObject({
      kind: 'request', name: 'papyrus.request', payload: { name: 'cost', args: [42] },
    });
    deliver(helper, { kind: 'reply', id: papyrusOutbound.id, payload: { value: 17 } });
    await expect(papyrus).resolves.toBe(17);
  });

  it('serves all four registry reads from replayed 2.0 state', async () => {
    const { helper, sent } = load('?osfui-api=1');
    const fixtures = [
      ['settings.get', 'settings', 'settings.data', { mods: [{ id: 'acme.widgets' }] }],
      ['views.get', 'views', 'views.data', { views: [{ id: 'acme.widgets/panel' }] }],
      ['i18n.get', 'i18n', 'i18n.data', { locale: 'en', strings: {} }],
      ['diagnostics.get', 'diagnostics', 'diagnostics.data', { issues: [] }],
    ] as const;
    for (const [, key, , value] of fixtures) {
      deliver(helper, { kind: 'state', mod: 'osfui', key, value });
    }
    const before = sent.length;
    for (const [command, , type, value] of fixtures) {
      const listener = vi.fn();
      helper.on(type, listener);
      const reply = await helper.request(command);
      expect(reply.type).toBe(type);
      expect(reply.payload).toEqual(value);
      expect(listener).toHaveBeenLastCalledWith(reply.payload, reply);
    }
    expect(sent).toHaveLength(before);

    const pushedRead = vi.fn();
    helper.on('views.data', pushedRead);
    expect(helper.send('views.get')).toBe(true);
    await vi.waitFor(() => {
      expect(pushedRead).toHaveBeenLastCalledWith(
        fixtures[1][3],
        { type: 'views.data', requestId: 'q5', payload: fixtures[1][3] },
      );
    });
    expect(sent).toHaveLength(before);
  });

  it('preserves legacy settings acknowledgements and human-time key capture', async () => {
    vi.useFakeTimers();
    try {
      const { helper, sent } = load('?osfui-api=1');
      const failedSet = helper.request('settings.set', { mod: 'acme.widgets', key: 'size', value: 99 });
      const setOutbound = last(sent);
      deliver(helper, {
        kind: 'error', id: setOutbound.id,
        payload: { code: 'invalid-value', message: 'outside range' },
      });
      await expect(failedSet).resolves.toEqual({
        type: 'settings.ack', requestId: 'q1',
        payload: {
          mod: 'acme.widgets', key: 'size', ok: false,
          code: 'invalid-value', message: 'outside range',
        },
      });

      const listener = vi.fn();
      helper.on('settings.captured', listener);
      const capture = helper.request(
        'settings.captureKey', { mod: 'acme.widgets', key: 'toggle' }, { timeoutMs: 0 },
      );
      const captureOutbound = last(sent);
      deliver(helper, {
        kind: 'reply', id: captureOutbound.id,
        payload: { armed: true, mod: 'acme.widgets', key: 'toggle' },
      });
      let settled = false;
      capture.then(() => { settled = true; });
      await Promise.resolve();
      expect(settled).toBe(false);
      deliver(helper, {
        kind: 'event', name: 'settings.captured',
        payload: { mod: 'acme.widgets', key: 'toggle', name: 'F8', cancelled: false },
      });
      const captured = await capture;
      expect(captured).toEqual({
        type: 'settings.captured', requestId: 'q2',
        payload: { mod: 'acme.widgets', key: 'toggle', name: 'F8', cancelled: false },
      });
      expect(listener).toHaveBeenCalledTimes(1);
      expect(listener).toHaveBeenCalledWith(captured.payload, captured);
    } finally {
      vi.useRealTimers();
    }
  });

  it('answers settings.reset with the refreshed legacy settings.data document', async () => {
    const { helper, sent } = load('?osfui-api=1');
    deliver(helper, {
      kind: 'state', mod: 'osfui', key: 'settings',
      value: { mods: [{ id: 'acme.widgets', values: { size: 9 } }] },
    });
    const reset = helper.request('settings.reset', { mod: 'acme.widgets', key: 'size' });
    const outbound = last(sent);
    deliver(helper, { kind: 'reply', id: outbound.id, payload: {} });
    let settled = false;
    reset.then(() => { settled = true; });
    await Promise.resolve();
    expect(settled).toBe(false);
    const refreshed = { mods: [{ id: 'acme.widgets', values: { size: 3 } }] };
    deliver(helper, { kind: 'state', mod: 'osfui', key: 'settings', value: refreshed });
    await expect(reset).resolves.toEqual({
      type: 'settings.data', requestId: 'q1', payload: refreshed,
    });
  });

  it('translates retained state and transient pushes into data.on/get', () => {
    const { helper } = load('?osfui-api=1');
    const values = vi.fn();
    helper.data.on('Inventory.Counts', values);
    deliver(helper, {
      kind: 'state', mod: 'acme.widgets', key: 'inventory.counts', value: ['aid', 'ammo'],
    });
    expect(values).toHaveBeenLastCalledWith(
      ['aid', 'ammo'],
      { mod: 'acme.widgets', key: 'inventory.counts', value: ['aid', 'ammo'] },
      {
        type: 'data.state',
        payload: { mod: 'acme.widgets', key: 'inventory.counts', value: ['aid', 'ammo'] },
      },
    );
    expect(helper.data.get('INVENTORY.COUNTS')).toEqual(['aid', 'ammo']);

    deliver(helper, {
      kind: 'event', name: 'data.push', payload: { key: 'Inventory.Counts', values: ['food'] },
    });
    expect(values).toHaveBeenLastCalledWith(
      ['food'],
      { key: 'Inventory.Counts', values: ['food'] },
      { type: 'data.push', payload: { key: 'Inventory.Counts', values: ['food'] } },
    );
    expect(helper.data.get('inventory.counts')).toEqual(['food']);
  });

  it('restores root i18n and theme helpers', async () => {
    const { helper } = load('?osfui-api=1');
    deliver(helper, {
      kind: 'state', mod: 'osfui', key: 'i18n',
      value: { locale: 'fr', strings: { greeting: 'Bonjour {name}' } },
    });
    await helper.i18nReady;
    expect(helper.locale()).toBe('fr');
    expect(helper.t('greeting', 'Hello {name}', { name: 'Sam' })).toBe('Bonjour Sam');
    const element = document.createElement('div');
    helper.applyAccent(element, '#123456');
    expect(element.style.getPropertyValue('--osf-accent')).toBe('#123456');
    const label = document.createElement('span');
    label.dataset.i18n = 'greeting';
    label.textContent = 'Hello {name}';
    document.body.append(label);
    helper.localize(document);
    expect(label.textContent).toBe('Bonjour {name}');
  });
});
