// @vitest-environment jsdom

import { describe, expect, it, vi } from 'vitest';
import { readFileSync } from 'node:fs';
import { resolve } from 'node:path';

const HELPER_SRC = readFileSync(resolve(process.cwd(), 'src/shared-kit/osfui.js'), 'utf8');

interface Frame {
  type: string;
  requestId?: string;
  payload: Record<string, unknown>;
}

interface Helper {
  emit(command: string, fields?: Record<string, unknown>): boolean;
  call<T>(command: string, fields?: Record<string, unknown>): Promise<T>;
  papyrus: {
    request<T>(name: string, ...args: Array<string | number | boolean>): Promise<T>;
  };
  data: {
    get<T>(key: string): T | undefined;
    on<T>(key: string, fn: (value: T) => void): () => void;
  };
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

function deliver(helper: Helper, message: unknown): void {
  helper.onMessage(JSON.stringify(message));
}

describe('author-friendly bridge helpers', () => {
  it('emit() is the fire-and-forget command spelling', () => {
    const { helper, sent } = loadHelper();
    expect(helper.emit('acme.mod.equip', { formId: 42 })).toBe(true);
    expect(sent).toEqual([{
      type: 'ui.command',
      payload: { command: 'acme.mod.equip', formId: 42 },
    }]);
  });

  it('call() resolves directly with a correlated reply payload', async () => {
    const { helper, sent } = loadHelper();
    const result = helper.call<{ weight: number }>('acme.mod.getWeight', { formId: 42 });
    const requestId = sent[0]!.requestId;

    // Qualified requests ignore the delivery acknowledgement.
    deliver(helper, { type: 'ui.result', requestId, payload: { ok: true } });
    deliver(helper, { type: 'acme.mod.weight', requestId, payload: { weight: 12.5 } });

    await expect(result).resolves.toEqual({ weight: 12.5 });
  });

  it('papyrus.request() sends scalar args and resolves directly to the typed value', async () => {
    const { helper, sent } = loadHelper();
    const result = helper.papyrus.request<number>('calculatePrice', 42, true);
    expect(sent[0]!.payload).toEqual({
      command: 'ui.papyrusRequest',
      request: 'calculatePrice',
      args: [42, true],
    });
    deliver(helper, {
      type: 'papyrus.result',
      requestId: sent[0]!.requestId,
      payload: { value: 125 },
    });
    await expect(result).resolves.toBe(125);
  });
  it('data caches typed state and replays it by case-insensitive key', () => {
    const { helper } = loadHelper();
    deliver(helper, {
      type: 'data.state',
      payload: { mod: 'acme.mod', key: 'Inventory.Counts', value: [2, 5] },
    });

    expect(helper.data.get<number[]>('inventory.counts')).toEqual([2, 5]);
    const listener = vi.fn();
    const off = helper.data.on<number[]>('INVENTORY.COUNTS', listener);
    expect(listener).toHaveBeenCalledOnce();
    expect(listener).toHaveBeenLastCalledWith([2, 5], expect.anything(), expect.anything());

    deliver(helper, {
      type: 'data.state',
      payload: { mod: 'acme.mod', key: 'inventory.counts', value: [8] },
    });
    expect(listener).toHaveBeenCalledTimes(2);
    expect(helper.data.get<number[]>('Inventory.Counts')).toEqual([8]);

    off();
    deliver(helper, {
      type: 'data.state',
      payload: { mod: 'acme.mod', key: 'inventory.counts', value: [13] },
    });
    expect(listener).toHaveBeenCalledTimes(2);
  });

  it('data.on also normalizes legacy values and forms pushes', () => {
    const { helper } = loadHelper();
    const strings = vi.fn();
    const forms = vi.fn();
    helper.data.on<string[]>('names', strings);
    helper.data.on<Array<{ formId: number }>>('forms', forms);

    deliver(helper, { type: 'data.push', payload: { mod: 'acme.mod', key: 'names', values: ['A'] } });
    deliver(helper, {
      type: 'data.push',
      payload: { mod: 'acme.mod', key: 'forms', values: [], forms: [{ formId: 42 }] },
    });

    expect(strings).toHaveBeenLastCalledWith(['A'], expect.anything(), expect.anything());
    expect(forms).toHaveBeenLastCalledWith([{ formId: 42 }], expect.anything(), expect.anything());
  });
});