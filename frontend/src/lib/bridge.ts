
import type { EventName, EventPayload, StateKey, StateValue, BridgeError } from './protocol';
import type { RuntimeInfo, JsonObject, I18nCatalog, PapyrusCallArgument } from '@sdk';

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

  /** Latest value of a state key, or undefined. Imperative escape hatch; prefer state(). */
  peek<T extends StateKey>(key: T): StateValue<T> | undefined;

  /** Resolves with OSF UI runtime handshake info. Rejects "no-bridge" standalone. */
  ready(): Promise<RuntimeInfo>;

  /** Resolves once the first i18n catalog has arrived (or failed over to English). */
  i18nReady(): Promise<I18nCatalog | { locale: string; strings: Record<string, string> }>;

  /** Active normalised locale ("en", "de", "pt-BR", ...). */
  locale(): string;

  /** Translate a structural address, falling back to the inline English. */
  t(address: string, english: string, vars?: Record<string, string | number>): string;

  /** Apply a mod accent hex to a subtree; a missing/invalid hex clears it. */
  applyAccent(el: HTMLElement, hex: string | null | undefined): void;

  /** Fire-and-forget call to an arbitrary GLOBAL Papyrus function. */
  papyrusCall(script: string, fn: string, ...args: PapyrusCallArgument[]): boolean;
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

export const windowBridge: Bridge = {
  available: () => window.osfui?.available === true,

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
    return on.call(window.osfui!.state, key, fn as (v: unknown) => void);
  },

  peek: <T extends StateKey>(key: T) =>
    window.osfui?.state?.get?.call(window.osfui!.state, key) as StateValue<T> | undefined,

  ready: () => window.osfui?.ready ?? Promise.reject(noBridgeError()),

  i18nReady: () => window.osfui?.i18n?.ready ?? Promise.resolve({ locale: 'en', strings: {} }),

  locale: () => window.osfui?.i18n?.locale ?? 'en',

  t: (address, english, vars) => {
    const t = window.osfui?.i18n?.t;
    if (t) return t.call(window.osfui!.i18n, address, english, vars);
    return interpolateEnglish(english, vars);
  },

  applyAccent: (el, hex) => {
    window.osfui?.theme?.applyAccent?.call(window.osfui!.theme, el, hex);
  },

  papyrusCall: (script, fn, ...args) => window.osfui?.papyrus?.call?.(script, fn, ...args) ?? false,
};

export const nullBridge: Bridge = {
  available: () => false,
  send: () => false,
  request: () => Promise.reject(noBridgeError()),
  on: () => () => {},
  onAny: () => () => {},
  state: () => () => {},
  peek: () => undefined,
  ready: () => Promise.reject(noBridgeError()),
  i18nReady: () => Promise.resolve({ locale: 'en', strings: {} }),
  locale: () => 'en',
  t: (_address, english, vars) => interpolateEnglish(english, vars),
  applyAccent: () => {},
  papyrusCall: () => false,
};
