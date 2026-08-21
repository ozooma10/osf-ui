// @vitest-environment jsdom

import { describe, expect, it, vi } from 'vitest';
import { readFileSync } from 'node:fs';
import { resolve } from 'node:path';
import type { OSFUIHelper, PapyrusEndpointPayload } from '@sdk';

const HELPER_SRC = readFileSync(resolve(process.cwd(), 'src/shared-kit/osfui.js'), 'utf8');

/** One web -> native envelope. Routing (kind/name/id) sits BESIDE the payload. */
interface Sent {
  kind: 'send' | 'request';
  name: string;
  id?: string;
  payload: Record<string, unknown>;
}

/** The helper owns the bridge transport's `onMessage` slot declared by the SDK. */
type Helper = OSFUIHelper & { onMessage(json: string): void };

function loadHelper(): { helper: Helper; sent: Sent[] } {
  const sent: Sent[] = [];
  (window as unknown as { osfui: unknown }).osfui = {
    postMessage(json: string) {
      sent.push(JSON.parse(json) as Sent);
    },
  };
  new Function(HELPER_SRC)();
  return { helper: window.osfui as unknown as Helper, sent };
}

function deliver(helper: Helper, message: unknown): void {
  helper.onMessage(JSON.stringify(message));
}

/** The newest envelope — i.e. past the handshake the helper posts on load. */
function lastPosted(sent: Sent[]): Sent {
  const envelope = sent[sent.length - 1];
  if (!envelope) throw new Error('the helper posted nothing');
  return envelope;
}

describe('author-friendly bridge helpers', () => {
  it('greets the OSF UI runtime itself, so first open and F5 are one boot path', () => {
    const { sent } = loadHelper();
    expect(sent).toEqual([{ kind: 'send', name: 'osfui.hello', payload: {} }]);
  });

  it('send() preserves the explicit object payload form and posts no id', () => {
    const { helper, sent } = loadHelper();
    const payload: PapyrusEndpointPayload = { args: [42] };
    expect(helper.send('equip', payload)).toBe(true);

    expect(lastPosted(sent)).toEqual({
      kind: 'send',
      name: 'equip',
      payload: { args: [42] },
    });
  });

  it('send() wraps direct Papyrus arguments in the portable endpoint payload', () => {
    const { helper, sent } = loadHelper();

    expect(helper.send('equip', 2)).toBe(true);
    expect(lastPosted(sent)).toEqual({
      kind: 'send',
      name: 'equip',
      payload: { args: [2] },
    });

    expect(helper.send('equip', 2, 3, 4)).toBe(true);
    expect(lastPosted(sent)).toEqual({
      kind: 'send',
      name: 'equip',
      payload: { args: [2, 3, 4] },
    });
  });

  it('request() correlates by id and resolves with the reply PAYLOAD', async () => {
    const { helper, sent } = loadHelper();
    const result = helper.request<number>('getWeight', { args: [42] });

    const envelope = lastPosted(sent);
    expect(envelope).toMatchObject({
      kind: 'request',
      name: 'getWeight',
      payload: { args: [42] },
    });
    // `id` is required on a request and is what the reply is matched against.
    expect(typeof envelope.id).toBe('string');
    expect(envelope.id).toBeTruthy();

    // The author awaits the payload, never the envelope.
    deliver(helper, { kind: 'reply', id: envelope.id, payload: 12.5 });
    await expect(result).resolves.toBe(12.5);
  });

  it('settles a request exactly once — a late second answer is ignored', async () => {
    const { helper, sent } = loadHelper();
    const result = helper.request<{ weight: number }>('acme.mod.getWeight');
    const { id } = lastPosted(sent);

    deliver(helper, { kind: 'reply', id, payload: { weight: 12.5 } });
    await expect(result).resolves.toEqual({ weight: 12.5 });

    const errors = vi.spyOn(console, 'error').mockImplementation(() => {});
    try {
      deliver(helper, { kind: 'error', id, payload: { code: 'boom', message: 'late' } });
      deliver(helper, { kind: 'reply', id, payload: { weight: 99 } });
      expect(errors).not.toHaveBeenCalled();
    } finally {
      errors.mockRestore();
    }
    await expect(result).resolves.toEqual({ weight: 12.5 });
  });

  it('uses the same request() surface for a local Papyrus endpoint', async () => {
    const { helper, sent } = loadHelper();
    const result = helper.request<number>('calculatePrice', { args: [42, true] });

    const envelope = lastPosted(sent);
    expect(envelope).toMatchObject({
      kind: 'request',
      name: 'calculatePrice',
      payload: { args: [42, true] },
    });

    deliver(helper, { kind: 'reply', id: envelope.id, payload: 125 });
    await expect(result).resolves.toBe(125);
  });

  it('request() wraps direct Papyrus arguments and resolves the raw reply', async () => {
    const { helper, sent } = loadHelper();
    const result = helper.request<number>('calculatePrice', 42, true, { timeoutMs: 0 });

    const envelope = lastPosted(sent);
    expect(envelope).toMatchObject({
      kind: 'request',
      name: 'calculatePrice',
      payload: { args: [42, true] },
    });

    deliver(helper, { kind: 'reply', id: envelope.id, payload: 125 });
    await expect(result).resolves.toBe(125);
  });

  it('exposes no unrelated convenience aliases alongside the compact author API', () => {
    const { helper } = loadHelper();
    for (const removed of ['available', 'ready', 'papyrus', 'i18n', 'theme']) {
      expect(removed in helper).toBe(false);
    }
    expect(typeof helper.state.get).toBe('function');
  });

  it('state caches values and replays them by case-insensitive key', () => {
    const { helper } = loadHelper();
    expect(helper.state.get('inventory.counts')).toBeUndefined();

    deliver(helper, { kind: 'ready', payload: { mod: 'acme.mod' } });
    deliver(helper, {
      kind: 'state',
      mod: 'acme.mod',
      key: 'Inventory.Counts',
      value: [2, 5],
    });
    expect(helper.state.get<number[]>('ACME.MOD/INVENTORY.COUNTS')).toEqual([2, 5]);
    expect(helper.state.get<number[]>('Inventory.Counts')).toEqual([2, 5]);

    const listener = vi.fn();
    const off = helper.state.on<number[]>('ACME.MOD/INVENTORY.COUNTS', listener);
    expect(listener).toHaveBeenCalledOnce();
    // Exactly one argument — the value. No envelope tail for authors to poke at.
    expect(listener).toHaveBeenLastCalledWith([2, 5]);

    const localListener = vi.fn();
    helper.state.on<number[]>('Inventory.Counts', localListener);
    expect(localListener).toHaveBeenLastCalledWith([2, 5]);

    deliver(helper, { kind: 'state', mod: 'acme.mod', key: 'inventory.counts', value: [8] });
    expect(helper.state.get<number[]>('inventory.counts')).toEqual([8]);
    expect(listener).toHaveBeenCalledTimes(2);
    expect(localListener).toHaveBeenCalledTimes(2);

    off();
    deliver(helper, { kind: 'state', mod: 'acme.mod', key: 'inventory.counts', value: [13] });
    expect(listener).toHaveBeenCalledTimes(2);
  });

  it('delivers a state value verbatim — no unwrapping, no per-shape rules', () => {
    const { helper } = loadHelper();
    const forms = vi.fn();
    helper.state.on('acme.mod/forms', forms);

    deliver(helper, {
      kind: 'state',
      mod: 'acme.mod',
      key: 'forms',
      value: { forms: [{ formId: 42 }] },
    });
    expect(forms).toHaveBeenLastCalledWith({ forms: [{ formId: 42 }] });

    deliver(helper, { kind: 'state', mod: 'acme.mod', key: 'forms', value: null });
    expect(forms).toHaveBeenLastCalledWith(null);
  });

  it('keeps mods separate: same key, two owners, two values', () => {
    const { helper } = loadHelper();
    const own = vi.fn();
    const other = vi.fn();
    helper.state.on<number[]>('acme.mod/counts', own);
    helper.state.on<number[]>('other.mod/counts', other);
    deliver(helper, { kind: 'state', mod: 'acme.mod', key: 'counts', value: [1] });
    deliver(helper, { kind: 'state', mod: 'other.mod', key: 'counts', value: [2] });

    expect(own).toHaveBeenLastCalledWith([1]);
    expect(other).toHaveBeenLastCalledWith([2]);
  });
});
