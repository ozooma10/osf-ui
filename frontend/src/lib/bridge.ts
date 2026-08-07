// Typed façade over the shipped `window.osfui` helper.
//
// Wraps the shipped helper (views/shared/osfui.js), never reimplements it: the
// helper owns onMessage, request correlation, the state cache, the i18n catalog
// and the page-initiated handshake, and is loaded by a classic <script> tag
// before this bundle runs. Forking any of that would fork the contract
// third-party views use.
//
// The global is `Partial<OSFUIHelper>` (it may be a bare injected bridge), so
// every member guards. Standalone, `available()` is false and `request()`
// rejects with code "no-bridge".

import type { EventName, EventPayload, StateKey, StateValue, BridgeError } from './protocol';
import type { RuntimeInfo, JsonObject, I18nCatalog, PapyrusArgument, PapyrusCallArgument } from '@sdk';

export interface RequestOptions {
  /**
   * Milliseconds before the request rejects with code "timeout". Default 10000.
   * `0` disables only the CLIENT timer; the OSF UI runtime still answers "no-response"
   * at its own 30 s deadline, so a request can no longer hang forever.
   */
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

  /** Subscribe to a mod-defined event (a `<mod>.<name>` the SDK cannot know about). */
  onAny<T = unknown>(event: string, fn: (payload: T) => void): () => void;

  /**
   * Subscribe to a named state value: the handler runs IMMEDIATELY with the
   * current value if one has arrived, and again on every change — including
   * after a reload, because state is replayed to every fresh document. This is
   * why a correct view needs no lifecycle code.
   */
  state<T extends StateKey>(key: T, fn: (value: StateValue<T>) => void): () => void;

  /** Latest value of a state key, or undefined. Imperative escape hatch; prefer state(). */
  peek<T extends StateKey>(key: T): StateValue<T> | undefined;

  /** Declare meaningful readiness for a manifest with readySignal:true. */
  markReady(): boolean;

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

  /** One-way message to the owning mod's Papyrus listener. */
  papyrusSend(name: string, ...args: PapyrusArgument[]): boolean;

  /** Fire-and-forget call to an arbitrary GLOBAL Papyrus function. */
  papyrusCall(script: string, fn: string, ...args: PapyrusCallArgument[]): boolean;

  /** Correlated request to the owning mod's Papyrus listener. */
  papyrusRequest<T = unknown>(name: string, ...args: PapyrusArgument[]): Promise<T>;
}

function noBridgeError(): BridgeError {
  const err = new Error('no bridge (standalone preview)') as BridgeError;
  err.code = 'no-bridge';
  return err;
}

/**
 * Interpolate `{name}` placeholders into the authored English — the fallback
 * both bridges use when the i18n helper is absent (plain-browser preview / unit
 * tests), so a view still renders readable text.
 */
function interpolateEnglish(english: string, vars?: Record<string, string | number>): string {
  return String(english ?? '').replace(/\{([A-Za-z0-9_]+)\}/g, (all, name: string) =>
    vars && Object.prototype.hasOwnProperty.call(vars, name) ? String(vars[name]) : all,
  );
}

/**
 * Reads the global the shared kit decorated. Every member degrades when the
 * helper is absent: this module is imported by the dev harness before the mock
 * installs, and by unit tests in plain node.
 */
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

  markReady: () => window.osfui?.markReady?.() ?? false,

  ready: () => window.osfui?.ready ?? Promise.reject(noBridgeError()),

  i18nReady: () => window.osfui?.i18n?.ready ?? Promise.resolve({ locale: 'en', strings: {} }),

  locale: () => window.osfui?.i18n?.locale ?? 'en',

  // Without the helper, interpolate the authored English so a view still
  // renders readable text (plain-browser preview).
  t: (address, english, vars) => {
    const t = window.osfui?.i18n?.t;
    if (t) return t.call(window.osfui!.i18n, address, english, vars);
    return interpolateEnglish(english, vars);
  },

  applyAccent: (el, hex) => {
    window.osfui?.theme?.applyAccent?.call(window.osfui!.theme, el, hex);
  },

  papyrusSend: (name, ...args) => window.osfui?.papyrus?.send?.(name, ...args) ?? false,

  papyrusCall: (script, fn, ...args) => window.osfui?.papyrus?.call?.(script, fn, ...args) ?? false,

  papyrusRequest: <T = unknown>(name: string, ...args: PapyrusArgument[]): Promise<T> => {
    const request = window.osfui?.papyrus?.request;
    if (!request) return Promise.reject(noBridgeError());
    return request.call(window.osfui!.papyrus, name, ...args) as Promise<T>;
  },
};

/**
 * Never-available bridge: the standalone/plain-browser case and the unit-test
 * default. Lives here, not in the harness, so production code can depend on it
 * without pulling dev-only modules into the graph.
 */
export const nullBridge: Bridge = {
  available: () => false,
  send: () => false,
  request: () => Promise.reject(noBridgeError()),
  on: () => () => {},
  onAny: () => () => {},
  state: () => () => {},
  peek: () => undefined,
  markReady: () => false,
  ready: () => Promise.reject(noBridgeError()),
  i18nReady: () => Promise.resolve({ locale: 'en', strings: {} }),
  locale: () => 'en',
  t: (_address, english, vars) => interpolateEnglish(english, vars),
  applyAccent: () => {},
  papyrusSend: () => false,
  papyrusCall: () => false,
  papyrusRequest: () => Promise.reject(noBridgeError()),
};
