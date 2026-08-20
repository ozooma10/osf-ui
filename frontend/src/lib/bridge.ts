
import type { EventName, EventPayload, StateKey, StateValue, BridgeError } from './protocol';
import type { JsonObject, I18nCatalog } from '@sdk';

export interface RequestOptions {
  timeoutMs?: number;
}

export interface Bridge {
  /** True when a native bridge (or the harness mock) is present. */
  available(): boolean;

  /** One-way. Returns "posted locally", never a remote outcome. */
  send(name: string, payload?: JsonObject): boolean;

  /** Settles exactly once with the reply PAYLOAD. Rejects with a {@link BridgeError}. */
  request<T = unknown>(name: string, payload?: JsonObject, opts?: RequestOptions): Promise<T>;

  /** Subscribe to a one-shot happening. Returns the unsubscribe fn. */
  on<T extends EventName>(event: T, fn: (payload: EventPayload<T>) => void): () => void;

  /** Subscribe to a mod-defined event the SDK cannot know about (local for own, qualified cross-mod). */
  onAny<T = unknown>(event: string, fn: (payload: T) => void): () => void;

  state<T extends StateKey>(key: T, fn: (value: StateValue<T>) => void): () => void;

  /** Translate a structural address, falling back to the inline English. */
  t(address: string, english: string, vars?: Record<string, string | number>): string;

  /** Apply a mod accent hex to a subtree; a missing/invalid hex clears it. */
  applyAccent(el: HTMLElement, hex: string | null | undefined): void;

}

function noBridgeError(): BridgeError {
  const err = new Error('no bridge (standalone preview)') as BridgeError;
  err.code = 'no-bridge';
  return err;
}

function interpolateEnglish(english: string, vars?: Record<string, string | number>): string {
  return String(english ?? '').replace(/\{([A-Za-z0-9_]+)\}/g, (all, name: string) =>
    vars && Object.prototype.hasOwnProperty.call(vars, name) ? String(vars[name]) : all,
  );
}

let locale = 'en';
let strings: Record<string, string> = {};

function adoptI18n(value: unknown): void {
  const catalog = value && typeof value === 'object' ? value as Partial<I18nCatalog> : {};
  locale = typeof catalog.locale === 'string' ? catalog.locale : 'en';
  strings = catalog.strings && typeof catalog.strings === 'object' ? catalog.strings : {};
  document.documentElement.lang = locale;
}

function translate(address: string, english: string, vars?: Record<string, string | number>): string {
  const value = Object.prototype.hasOwnProperty.call(strings, address) ? strings[address] : english;
  return interpolateEnglish(value ?? '', vars);
}

const ACCENT_TOKENS = [
  '--osf-accent',
  '--osf-accent-hover',
  '--osf-accent-quiet',
  '--osf-accent-strong',
];

function applyAccent(el: HTMLElement, hex: string | null | undefined): void {
  if (typeof hex !== 'string' || !/^#[0-9a-fA-F]{6}([0-9a-fA-F]{2})?$/.test(hex)) {
    for (const token of ACCENT_TOKENS) el.style.removeProperty(token);
    return;
  }
  const rgb = [1, 3, 5].map((index) => parseInt(hex.slice(index, index + 2), 16));
  const mix = (target: number, amount: number) => '#' + rgb.map((channel) =>
    Math.round(channel + (target - channel) * amount).toString(16).padStart(2, '0'),
  ).join('');
  el.style.setProperty('--osf-accent', hex.slice(0, 7));
  el.style.setProperty('--osf-accent-hover', mix(255, 0.34));
  el.style.setProperty('--osf-accent-strong', mix(0, 0.42));
  el.style.setProperty('--osf-accent-quiet', `rgba(${rgb[0]}, ${rgb[1]}, ${rgb[2]}, 0.14)`);
}

export const windowBridge: Bridge = {
  available: () => typeof window.osfui?.postMessage === 'function',

  send: (name, payload) => window.osfui?.send?.(name, payload) ?? false,

  request: <T = unknown>(name: string, payload?: JsonObject, opts?: RequestOptions): Promise<T> => {
    const request = window.osfui?.request;
    if (!request) return Promise.reject(noBridgeError());
    return request.call(window.osfui, name, payload, opts) as Promise<T>;
  },

  on: <T extends EventName>(event: T, fn: (payload: EventPayload<T>) => void) => {
    const on = window.osfui?.on;
    if (!on) return () => {};
    return on.call(window.osfui, event, fn as (p: unknown) => void);
  },

  onAny: <T = unknown>(event: string, fn: (payload: T) => void) => {
    const on = window.osfui?.on;
    if (!on) return () => {};
    return on.call(window.osfui, event, fn as (p: unknown) => void);
  },

  state: <T extends StateKey>(key: T, fn: (value: StateValue<T>) => void) => {
    const on = window.osfui?.state?.on;
    if (!on) return () => {};
    return on.call(window.osfui!.state, key, (value: unknown) => {
      if (String(key).toLowerCase() === 'osfui/i18n') adoptI18n(value);
      fn(value as StateValue<T>);
    });
  },

  t: translate,
  applyAccent,
};

export const nullBridge: Bridge = {
  available: () => false,
  send: () => false,
  request: () => Promise.reject(noBridgeError()),
  on: () => () => {},
  onAny: () => () => {},
  state: () => () => {},
  t: (_address, english, vars) => interpolateEnglish(english, vars),
  applyAccent: () => {},
};
