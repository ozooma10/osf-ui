// nullBridge — the "there is no native bridge" façade used by browser preview
// and unit tests: inert, but never throws and never hangs an awaiting caller.
// Runs in the node environment so a stray `window` access fails the test.

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
    // Guards the "no globals in src/lib" rule: `window` is undefined here, so
    // any stray access would ReferenceError.
    expect(typeof globalThis.window).toBe('undefined');
    expect(nullBridge.available()).toBe(false);
  });
});

describe('nullBridge — one-way members', () => {
  it('return false instead of throwing', () => {
    // Views check the boolean to show an offline notice; throwing would take
    // the whole render down in standalone preview. `send` returns "posted
    // locally", so false here is the honest answer, not an error swallowed.
    expect(nullBridge.send('close')).toBe(false);
    expect(nullBridge.send('setVisible', { visible: false })).toBe(false);
    expect(nullBridge.markReady()).toBe(false);
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
    // Same contract as the shipped helper's local rejections (timeout,
    // no-bridge): `"payload" in err` distinguishes "the OSF UI runtime refused,
    // and here is its code" from "we gave up / there is no bridge".
    expect('payload' in (await caught(nullBridge.request('ping')))).toBe(false);
  });

  it('rejects immediately rather than waiting out a timeout', async () => {
    // No deadline is honoured; even the "client timer disabled" option rejects
    // at once, because there is nothing that could ever answer.
    await expect(nullBridge.request('game.get', {}, { timeoutMs: 0 })).rejects.toThrow();
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

    // Unsubscribing answers nothing — matching the shipped helper, whose
    // unsubscribe is a statement body. Covered so a caller who starts branching
    // on the return value hits this instead of a standalone-only bug.
    expect(off()).toBeUndefined();

    // Mod-defined events (`<mod>.<name>`) go through the same inert path.
    expect(() => nullBridge.onAny('acme.mod.looted', never)()).not.toThrow();
  });

  it('subscribes to state without replaying anything', () => {
    // The real helper replays the current value SYNCHRONOUSLY on subscribe, so
    // views put their first render in that handler. Standalone there is no
    // value: the handler must simply never run, rather than run with undefined
    // and make every view render an empty registry as if it were real.
    const handler = vi.fn();
    const off = nullBridge.state('osfui/settings', handler);

    expect(handler).not.toHaveBeenCalled();
    expect(typeof off).toBe('function');
    expect(() => off()).not.toThrow();
  });

  it('peeks undefined for every key', () => {
    // Distinguishable from a delivered value: `undefined` is "nothing has
    // arrived", which is exactly true standalone.
    expect(nullBridge.peek('osfui/settings')).toBeUndefined();
    expect(nullBridge.peek('osfui/views')).toBeUndefined();
  });
});

describe('nullBridge — ready / i18n', () => {
  it('REJECTS ready() with "no-bridge" rather than hanging', async () => {
    // 1.x left this pending forever, so a view that awaited it before its first
    // paint rendered nothing in a plain browser and gave the author no clue
    // why. Rejecting fails fast with a code the view can branch on, and still
    // never claims an OSF UI release version the preview does not have.
    const err = await caught(nullBridge.ready());
    expect(err.code).toBe('no-bridge');
    expect(err.message).toBe('no bridge (standalone preview)');
  });

  it('resolves i18nReady immediately with the empty English catalog', async () => {
    // Opposite of ready(): views await i18nReady before first paint, so leaving
    // it pending would render nothing in a plain browser.
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
    // Not blanked: a visible "{name}" reads as an authoring bug, an empty gap
    // reads as finished copy.
    expect(nullBridge.t('a', 'Hello, {name}!')).toBe('Hello, {name}!');
    expect(nullBridge.t('a', '{a} and {b}', { a: 'x' })).toBe('x and {b}');
  });

  it('ignores inherited properties when resolving a placeholder', () => {
    // `hasOwnProperty` guard: "toString" is on Object.prototype and must not be
    // substituted into user-facing copy.
    expect(nullBridge.t('a', '{toString}')).toBe('{toString}');
  });

  it('only matches [A-Za-z0-9_] placeholder names', () => {
    // The pattern excludes dots and spaces, so `{mod.id}` and `{ name }` pass
    // through untouched.
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
