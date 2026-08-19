
import { describe, it, expect, vi } from 'vitest';
import { nullBridge } from '@lib/bridge';


interface CaughtError extends Error {
  code?: unknown;
  payload?: unknown;
}

/** Await a promise that must reject and hand back the error, typed. */
async function caught(promise: Promise<unknown>): Promise<CaughtError> {
  try {
    await promise;
  } catch (e) {
    return e as CaughtError;
  }
  throw new Error('expected the call to reject');
}

describe('nullBridge — presence', () => {
  it('reports itself unavailable', () => {
    expect(nullBridge.available()).toBe(false);
  });

  it('does not require a DOM to import or call', () => {
    expect(typeof globalThis.window).toBe('undefined');
    expect(nullBridge.available()).toBe(false);
  });
});

describe('nullBridge — one-way members', () => {
  it('return false instead of throwing', () => {
    expect(nullBridge.send('close')).toBe(false);
    expect(nullBridge.send('setVisible', { visible: false })).toBe(false);
    expect(nullBridge.papyrusCall('AcmeWidgets', 'Refresh')).toBe(false);
    expect(nullBridge.papyrusSend('doorOpened', 'airlock', 3)).toBe(false);
  });
});

describe('nullBridge — request', () => {
  it('rejects with code "no-bridge" and the standalone-preview message', async () => {
    const err = await caught(nullBridge.request('ping'));

    expect(err).toBeInstanceOf(Error);
    expect(err.code).toBe('no-bridge');
    expect(err.message).toBe('no bridge (standalone preview)');
  });

  it('omits `payload` — the error is synthesised, not a message', async () => {
    expect('payload' in (await caught(nullBridge.request('ping')))).toBe(false);
  });

  it('rejects immediately rather than waiting out a timeout', async () => {
    await expect(nullBridge.request('ping', {}, { timeoutMs: 0 })).rejects.toThrow();
  });

  it('makes a FRESH error per call, so a caller may annotate it safely', async () => {
    const a = await caught(nullBridge.request('ping'));
    const b = await caught(nullBridge.request('ping'));
    expect(a).not.toBe(b);
  });

  it('rejects papyrusRequest the same way — sugar is not a second contract', async () => {
    const err = await caught(nullBridge.papyrusRequest('calculatePrice', 42));
    expect(err.code).toBe('no-bridge');
  });
});

describe('nullBridge — on / state', () => {
  it('returns a no-op unsubscribe that is safe to call twice', () => {
    const never = () => {
      throw new Error('a null bridge must never deliver a message');
    };
    const off = nullBridge.on('settings.changed', never);

    expect(typeof off).toBe('function');
    expect(() => off()).not.toThrow();
    expect(() => off()).not.toThrow();

    expect(off()).toBeUndefined();

    // Mod-defined events (`<mod>.<name>`) go through the same inert path.
    expect(() => nullBridge.onAny('acme.mod.looted', never)()).not.toThrow();
  });

  it('subscribes to state without replaying anything', () => {
    const handler = vi.fn();
    const off = nullBridge.state('osfui/settings', handler);

    expect(handler).not.toHaveBeenCalled();
    expect(typeof off).toBe('function');
    expect(() => off()).not.toThrow();
  });

  it('peeks undefined for every key', () => {
    expect(nullBridge.peek('osfui/settings')).toBeUndefined();
    expect(nullBridge.peek('osfui/views')).toBeUndefined();
  });
});

describe('nullBridge — ready / i18n', () => {
  it('REJECTS ready() with "no-bridge" rather than hanging', async () => {
    const err = await caught(nullBridge.ready());
    expect(err.code).toBe('no-bridge');
    expect(err.message).toBe('no bridge (standalone preview)');
  });

  it('resolves i18nReady immediately with the empty English catalog', async () => {
    await expect(nullBridge.i18nReady()).resolves.toEqual({ locale: 'en', strings: {} });
  });

  it('reports locale "en"', () => {
    expect(nullBridge.locale()).toBe('en');
  });
});

describe('nullBridge — t()', () => {
  it('returns the authored English when there is nothing to interpolate', () => {
    expect(nullBridge.t('settings.title', 'Settings')).toBe('Settings');
  });

  it('interpolates {name} placeholders from vars', () => {
    expect(
      nullBridge.t('settings.count', '{count} of {total} mods', { count: 2, total: 7 }),
    ).toBe('2 of 7 mods');
  });

  it('stringifies non-string vars', () => {
    expect(nullBridge.t('a', 'v={n}', { n: 0 })).toBe('v=0');
  });

  it('leaves an UNMATCHED placeholder literal', () => {
    expect(nullBridge.t('a', 'Hello, {name}!')).toBe('Hello, {name}!');
    expect(nullBridge.t('a', '{a} and {b}', { a: 'x' })).toBe('x and {b}');
  });

  it('ignores inherited properties when resolving a placeholder', () => {
    expect(nullBridge.t('a', '{toString}')).toBe('{toString}');
  });

  it('only matches [A-Za-z0-9_] placeholder names', () => {
    expect(nullBridge.t('a', '{mod.id} / { name } / {a-b}', { name: 'x' })).toBe(
      '{mod.id} / { name } / {a-b}',
    );
  });

  it('coerces a null/undefined English to the empty string', () => {
    // `String(english ?? "")`: a missing default renders blank, not "undefined".
    expect(nullBridge.t('a', undefined as unknown as string)).toBe('');
    expect(nullBridge.t('a', null as unknown as string)).toBe('');
  });
});

describe('nullBridge — applyAccent', () => {
  it('is a no-op that tolerates any argument, including null', () => {
    expect(() => nullBridge.applyAccent(null as unknown as HTMLElement, '#3aa9c0')).not.toThrow();
    expect(() => nullBridge.applyAccent(null as unknown as HTMLElement, null)).not.toThrow();
  });
});
