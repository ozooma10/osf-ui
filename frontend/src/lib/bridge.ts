// Typed façade over the shipped `window.osfui` helper.
//
// Wraps the frozen helper (data/OSFUI/views/shared/osfui.js), never reimplements
// it: the helper owns onMessage, request/reply correlation, the i18n catalog and
// the ready handshake, and is loaded by a classic <script> tag before this
// bundle runs. Forking any of that would fork the contract third-party views use.
//
// The global is `Partial<OSFUIHelper>` (it may be a bare injected bridge), so
// every member guards. Standalone, `available()` is false and `request()`
// rejects with code "no-bridge".

import type { NativeMessageType, PayloadOf, BridgeError } from './protocol';
import type { NativeToWebMessage, RuntimeReadyPayload } from '@sdk';

export interface RequestOptions {
  /**
   * Milliseconds before the request rejects with code "timeout". Default 10000.
   * Pass 0 to disable — required for `settings.captureKey`, which waits on a
   * keypress and has no deadline.
   */
  timeoutMs?: number;
}

export interface Bridge {
  /** True when a native bridge (or the harness mock) is present. */
  available(): boolean;
  /** Fire-and-forget. Returns false when no bridge is present. */
  send(command: string, fields?: Record<string, unknown>): boolean;
  /** Correlated request. Rejects with a {@link BridgeError}. */
  request<T extends NativeToWebMessage = NativeToWebMessage>(
    command: string,
    fields?: Record<string, unknown>,
    opts?: RequestOptions,
  ): Promise<T>;
  /** Subscribe to a native->web message type. Returns the unsubscribe fn. */
  on<T extends NativeMessageType>(
    type: T,
    fn: (payload: PayloadOf<T>, message: Extract<NativeToWebMessage, { type: T }>) => void,
  ): () => void;
  /** Resolves with the runtime.ready payload. Never resolves standalone. */
  ready(): Promise<RuntimeReadyPayload>;
  /** Resolves once the first i18n catalog has arrived (or failed over to English). */
  i18nReady(): Promise<unknown>;
  /** Active normalised locale ("en", "de", "pt-BR", ...). */
  locale(): string;
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

/**
 * Reads the global the shared kit decorated. Every member degrades when the
 * helper is absent: this module is imported by the dev harness before the mock
 * installs, and by unit tests in plain node.
 */
export const windowBridge: Bridge = {
  available: () => !!window.osfui?.available?.(),

  send: (command, fields) => window.osfui?.send?.(command, fields) ?? false,

  request: <T extends NativeToWebMessage = NativeToWebMessage>(
    command: string,
    fields?: Record<string, unknown>,
    opts?: RequestOptions,
  ): Promise<T> => {
    const req = window.osfui?.request;
    if (!req) return Promise.reject(noBridgeError());
    return req.call(window.osfui, command, fields, opts) as Promise<T>;
  },

  on: <T extends NativeMessageType>(
    type: T,
    fn: (payload: PayloadOf<T>, message: Extract<NativeToWebMessage, { type: T }>) => void,
  ) => {
    const on = window.osfui?.on;
    if (!on) return () => {};
    return on.call(window.osfui, type, fn as (p: unknown, m: NativeToWebMessage) => void);
  },

  ready: () => window.osfui?.ready ?? new Promise<RuntimeReadyPayload>(() => {}),

  i18nReady: () => window.osfui?.i18nReady ?? Promise.resolve({ locale: 'en', strings: {} }),

  locale: () => window.osfui?.locale?.() ?? 'en',

  // Without the helper, interpolate the authored English so a view still
  // renders readable text (plain-browser preview).
  t: (address, english, vars) => {
    const t = window.osfui?.t;
    if (t) return t.call(window.osfui, address, english, vars);
    return String(english ?? '').replace(/\{([A-Za-z0-9_]+)\}/g, (all, name: string) =>
      vars && Object.prototype.hasOwnProperty.call(vars, name) ? String(vars[name]) : all,
    );
  },

  applyAccent: (el, hex) => {
    const apply = (window.osfui as { applyAccent?: (e: HTMLElement, h: unknown) => void } | undefined)
      ?.applyAccent;
    apply?.call(window.osfui, el, hex);
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
  ready: () => new Promise(() => {}),
  i18nReady: () => Promise.resolve({ locale: 'en', strings: {} }),
  locale: () => 'en',
  t: (_address, english, vars) =>
    String(english ?? '').replace(/\{([A-Za-z0-9_]+)\}/g, (all, name: string) =>
      vars && Object.prototype.hasOwnProperty.call(vars, name) ? String(vars[name]) : all,
    ),
  applyAccent: () => {},
};
