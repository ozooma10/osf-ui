// @vitest-environment jsdom
//
// Pins the AUTHOR-facing surface of the shipped helper by loading the real
// src/shared-kit/osfui.js: third-party views get this exact object, so the
// spellings below are the published mod API 2.0 contract, not an internal.

import { describe, expect, it, vi } from 'vitest';
import { readFileSync } from 'node:fs';
import { resolve } from 'node:path';
import type { OSFUIHelper } from '@sdk';

const HELPER_SRC = readFileSync(resolve(process.cwd(), 'src/shared-kit/osfui.js'), 'utf8');

/** One web -> native envelope. Routing (kind/name/id) sits BESIDE the payload. */
interface Sent {
  kind: 'send' | 'request';
  name: string;
  id?: string;
  payload: Record<string, unknown>;
}

/** The helper owns the `onMessage` slot the SDK declares as the host's. */
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
  it('greets the host itself, so first open and F5 are one boot path', () => {
    // Page-initiated handshake: the helper sends osfui.hello the moment it
    // loads, and the host answers `ready` + a full state replay. Nothing in a
    // view has to "re-request my data on reload" — which is why every other
    // test here reads the envelope AFTER this one.
    const { sent } = loadHelper();
    expect(sent).toEqual([{ kind: 'send', name: 'osfui.hello', payload: {} }]);
  });

  it('send() is the fire-and-forget spelling, and posts no id', () => {
    const { helper, sent } = loadHelper();
    expect(helper.send('acme.mod.equip', { formId: 42 })).toBe(true);

    // The author's payload travels VERBATIM: 1.x smuggled the endpoint name
    // into it as a `command` field, which let a payload key override routing.
    expect(lastPosted(sent)).toEqual({
      kind: 'send',
      name: 'acme.mod.equip',
      payload: { formId: 42 },
    });
  });

  it('request() correlates by id and resolves with the reply PAYLOAD', async () => {
    const { helper, sent } = loadHelper();
    const result = helper.request<{ weight: number }>('acme.mod.getWeight', { formId: 42 });

    const envelope = lastPosted(sent);
    expect(envelope).toMatchObject({
      kind: 'request',
      name: 'acme.mod.getWeight',
      payload: { formId: 42 },
    });
    // `id` is required on a request and is what the reply is matched against.
    expect(typeof envelope.id).toBe('string');
    expect(envelope.id).toBeTruthy();

    // The author awaits the payload, never the envelope.
    deliver(helper, { kind: 'reply', id: envelope.id, payload: { weight: 12.5 } });
    await expect(result).resolves.toEqual({ weight: 12.5 });
  });

  it('settles a request exactly once — a late second answer is ignored', async () => {
    const { helper, sent } = loadHelper();
    const result = helper.request<{ weight: number }>('acme.mod.getWeight');
    const { id } = lastPosted(sent);

    deliver(helper, { kind: 'reply', id, payload: { weight: 12.5 } });
    await expect(result).resolves.toEqual({ weight: 12.5 });

    // A duplicate/late settlement must be dropped silently rather than
    // reported: the pending entry is gone, so there is nothing to reject and
    // nothing an author could have done about it.
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

  it('papyrus.request() sends scalar args and resolves to the typed value', async () => {
    const { helper, sent } = loadHelper();
    const result = helper.papyrus.request<number>('calculatePrice', 42, true);

    const envelope = lastPosted(sent);
    expect(envelope).toMatchObject({
      kind: 'request',
      name: 'papyrus.request',
      payload: { name: 'calculatePrice', args: [42, true] },
    });

    // The wire carries { value } so a Papyrus None is representable; the sugar
    // unwraps it, so the author sees the scalar the script returned.
    deliver(helper, { kind: 'reply', id: envelope.id, payload: { value: 125 } });
    await expect(result).resolves.toBe(125);
  });

  it('papyrus.send() is the one-way sibling and never correlates', () => {
    const { helper, sent } = loadHelper();
    expect(helper.papyrus.send('doorOpened', 'airlock', 3)).toBe(true);
    expect(lastPosted(sent)).toEqual({
      kind: 'send',
      name: 'papyrus.send',
      payload: { name: 'doorOpened', args: ['airlock', 3] },
    });
  });

  it('papyrus.call() targets an arbitrary GLOBAL function', () => {
    const { helper, sent } = loadHelper();
    expect(helper.papyrus.call('AcmeWidgets', 'Bump', 1, true)).toBe(true);
    expect(lastPosted(sent)).toEqual({
      kind: 'send',
      name: 'papyrus.call',
      payload: { script: 'AcmeWidgets', function: 'Bump', args: [1, true] },
    });
  });

  it('state caches values and replays them by case-insensitive key', () => {
    const { helper } = loadHelper();
    // Papyrus string interning means a key can arrive cased differently than
    // the script authored it, so lookup folds case on both sides.
    deliver(helper, {
      kind: 'state',
      mod: 'acme.mod',
      key: 'Inventory.Counts',
      value: [2, 5],
    });

    expect(helper.state.get<number[]>('acme.mod/inventory.counts')).toEqual([2, 5]);

    const listener = vi.fn();
    const off = helper.state.on<number[]>('ACME.MOD/INVENTORY.COUNTS', listener);
    // Subscribing is a READ: the current value replays synchronously, so a view
    // never has to ask "has it arrived yet?".
    expect(listener).toHaveBeenCalledOnce();
    // Exactly one argument — the value. No envelope tail for authors to poke at.
    expect(listener).toHaveBeenLastCalledWith([2, 5]);

    deliver(helper, { kind: 'state', mod: 'acme.mod', key: 'inventory.counts', value: [8] });
    expect(listener).toHaveBeenCalledTimes(2);
    expect(helper.state.get<number[]>('Acme.Mod/Inventory.Counts')).toEqual([8]);

    off();
    deliver(helper, { kind: 'state', mod: 'acme.mod', key: 'inventory.counts', value: [13] });
    expect(listener).toHaveBeenCalledTimes(2);
    // Unsubscribing stops delivery, not caching: the value is still current.
    expect(helper.state.get<number[]>('acme.mod/inventory.counts')).toEqual([13]);
  });

  it('delivers a state value verbatim — no unwrapping, no per-shape rules', () => {
    // 1.x inspected the push payload for `value` / `values` / `forms` and handed
    // the author whichever it found. 2.0 carries one opaque value beside the
    // routing, so a Papyrus forms list and a scalar travel the same path.
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
    // The cache key is "<mod>/<key>", so one mod's state can never clobber
    // another's — the reason state is addressed by owner at all.
    const { helper } = loadHelper();
    deliver(helper, { kind: 'state', mod: 'acme.mod', key: 'counts', value: [1] });
    deliver(helper, { kind: 'state', mod: 'other.mod', key: 'counts', value: [2] });

    expect(helper.state.get<number[]>('acme.mod/counts')).toEqual([1]);
    expect(helper.state.get<number[]>('other.mod/counts')).toEqual([2]);
  });
});
