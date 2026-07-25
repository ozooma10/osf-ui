// Typed façade over the shipped `window.osfui` helper.
//
// Wraps the frozen helper (data/OSFUI/views/shared/osfui.js), never reimplements
// it: the helper owns onMessage, request/reply correlation, the i18n catalog and
// the ready handshake, and is loaded by a classic <script> tag before this
// bundle runs. Forking any of that would fork the contract third-party views use.
//
// The global is `Partial<OSFUIHelper>` (it may be a bare injected bridge), so
// every member guards. Standalone, `available()` is false and `call()`
// rejects with code "no-bridge".

import type { NativeMessageType, PayloadOf, BridgeError } from './protocol';
import type { NativeToWebMessage, RuntimeReadyPayload } from '@sdk';

export interface CallOptions {
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
  /** Fire-and-forget command. Returns false when no bridge is present. */
  emit(command: string, fields?: Record<string, unknown>): boolean;
  /** Declare meaningful readiness for a manifest with readySignal:true. */
  viewReady(): boolean;
  /** Correlated request returning its payload directly. Rejects with a {@link BridgeError}. */
  call<TPayload = unknown>(
    command: string,
    fields?: Record<string, unknown>,
    opts?: CallOptions,
  ): Promise<TPayload>;
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
  available: () => !!window.osfui?.available?.(),

  emit: (command, fields) => window.osfui?.emit?.(command, fields) ?? false,

  viewReady: () => window.osfui?.viewReady?.() ?? false,

  call: <TPayload = unknown>(
    command: string,
    fields?: Record<string, unknown>,
    opts?: CallOptions,
  ): Promise<TPayload> => {
    const call = window.osfui?.call;
    if (!call) return Promise.reject(noBridgeError());
    return call.call(window.osfui, command, fields, opts) as Promise<TPayload>;
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
    return interpolateEnglish(english, vars);
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
  emit: () => false,
  viewReady: () => false,
  call: () => Promise.reject(noBridgeError()),
  on: () => () => {},
  ready: () => new Promise(() => {}),
  i18nReady: () => Promise.resolve({ locale: 'en', strings: {} }),
  locale: () => 'en',
  t: (_address, english, vars) => interpolateEnglish(english, vars),
  applyAccent: () => {},
};
