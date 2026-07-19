// nullBridge — the "there is no native bridge" façade.
//
// This is what runs in a plain browser preview and in every unit test, so its
// job is to be inert but never to throw and never to hang a caller that is
// awaiting it. Deliberately a NODE-environment test: nullBridge must not touch
// `window`, and running it without a DOM proves that.

import { describe, it, expect } from 'vitest';
import { nullBridge } from '@lib/bridge';
import { isBridgeError } from '@lib/protocol';

interface CaughtError extends Error {
  code?: unknown;
  reply?: unknown;
}

/** Await a promise that must REJECT and hand back the error, typed. */
async function caught(promise: Promise<unknown>): Promise<CaughtError> {
  try {
    await promise;
  } catch (e) {
    return e as CaughtError;
  }
  throw new Error('expected the request to reject');
}

describe('nullBridge — presence', () => {
  it('reports itself unavailable', () => {
    expect(nullBridge.available()).toBe(false);
  });

  it('does not require a DOM to import or call', () => {
    // Guards the "no globals in src/lib" rule: in vitest's node environment
    // `window` is undefined, so any stray access here would ReferenceError.
    expect(typeof globalThis.window).toBe('undefined');
    expect(nullBridge.available()).toBe(false);
  });
});

describe('nullBridge — send', () => {
  it('returns false instead of throwing', () => {
    // Views check the boolean to decide whether to show an offline notice;
    // throwing would take the whole render down in standalone preview.
    expect(nullBridge.send('close')).toBe(false);
    expect(nullBridge.send('settings.set', { mod: 'm', key: 'k', value: 1 })).toBe(false);
  });
});

describe('nullBridge — request', () => {
  it('rejects with code "no-bridge" and the standalone-preview message', async () => {
    const err = await caught(nullBridge.request('ping'));

    expect(err).toBeInstanceOf(Error);
    expect(err.code).toBe('no-bridge');
    expect(err.message).toBe('no bridge (standalone preview)');
    expect(isBridgeError(err)).toBe(true);
  });

  it('omits `reply` — the error is synthesised, not a message', async () => {
    // Same contract as the shipped helper's local rejections (timeout and
    // no-bridge): `"reply" in err` distinguishes "the host refused" from
    // "we gave up / there is no host".
    expect('reply' in (await caught(nullBridge.request('ping')))).toBe(false);
  });

  it('rejects immediately rather than waiting out a timeout', async () => {
    // No deadline is honoured because there is nothing to wait for; even the
    // "timeout disabled" option rejects at once.
    await expect(nullBridge.request('settings.captureKey', {}, { timeoutMs: 0 })).rejects.toThrow();
  });

  it('makes a FRESH error per call, so a caller may annotate it safely', async () => {
    const a = await caught(nullBridge.request('ping'));
    const b = await caught(nullBridge.request('ping'));
    expect(a).not.toBe(b);
  });
});

describe('nullBridge — on', () => {
  it('returns a no-op unsubscribe that is safe to call twice', () => {
    const off = nullBridge.on('settings.data', () => {
      throw new Error('a null bridge must never deliver a message');
    });

    expect(typeof off).toBe('function');
    expect(() => off()).not.toThrow();
    expect(() => off()).not.toThrow();

    // DIVERGENCE, pinned deliberately: nullBridge's unsubscribe returns
    // undefined, whereas the shipped helper's returns `set.delete(fn)` — a
    // boolean (shared-kit/osfui.js:119). Nothing reads that return value today
    // (the legacy views discard it), so the two are interchangeable in practice;
    // asserted here so a caller who ever starts depending on the boolean finds
    // this note instead of a silent standalone-only bug.
    expect(off()).toBeUndefined();
  });
});

describe('nullBridge — ready / i18n', () => {
  it('never resolves ready() — standalone has no runtime.ready', async () => {
    // Views gate on ready() to know the host version; resolving with a fake one
    // would make a preview claim capabilities it does not have. A forever-
    // pending promise is the deliberate choice (bridge.ts:46).
    const sentinel = Symbol('pending');
    const settled = await Promise.race([
      nullBridge.ready().then(() => 'resolved' as const),
      Promise.resolve(sentinel),
    ]);
    expect(settled).toBe(sentinel);
  });

  it('resolves i18nReady immediately with the empty English catalog', async () => {
    // The opposite choice from ready(), and for a concrete reason: views AWAIT
    // i18nReady before their first paint. Leaving it pending would render
    // nothing at all in a plain browser.
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
    // Not blanked: a visible "{name}" in the UI is a legible authoring bug,
    // whereas an empty gap silently reads as finished copy.
    expect(nullBridge.t('a', 'Hello, {name}!')).toBe('Hello, {name}!');
    expect(nullBridge.t('a', '{a} and {b}', { a: 'x' })).toBe('x and {b}');
  });

  it('ignores inherited properties when resolving a placeholder', () => {
    // `hasOwnProperty` guard: "toString" is on Object.prototype but must not be
    // substituted into user-facing copy.
    expect(nullBridge.t('a', '{toString}')).toBe('{toString}');
  });

  it('only matches [A-Za-z0-9_] placeholder names', () => {
    // `{mod.id}` and `{ name }` are not placeholders — the pattern excludes
    // dots and spaces, so such text passes through untouched.
    expect(nullBridge.t('a', '{mod.id} / { name } / {a-b}', { name: 'x' })).toBe(
      '{mod.id} / { name } / {a-b}',
    );
  });

  it('coerces a null/undefined English to the empty string', () => {
    // `String(english ?? "")`, so a missing default renders blank rather than
    // the literal text "undefined".
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
