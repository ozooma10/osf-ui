// bridge.ts — a typed façade over the shipped `window.osfui` helper.
//
// This WRAPS the frozen helper (data/OSFUI/views/shared/osfui.js); it does not
// reimplement it. The helper owns `onMessage`, request/reply correlation, the
// i18n catalog and the ready handshake, and it is loaded by a classic <script>
// tag before this bundle runs. Reimplementing any of that here would fork the
// public contract that third-party views also depend on.
//
// The façade exists for three reasons:
//   1. Types. The global is `Partial<OSFUIHelper>` because it may be a bare
//      injected bridge; every call site would otherwise need the same guards.
//   2. Testability. `Bridge` is an interface, so pure logic and components can
//      be exercised without a global.
//   3. Standalone safety. `available()` is false in a plain browser, and the
//      helper rejects `request()` immediately with code "no-bridge". Callers
//      get one documented failure mode instead of three.

import type { NativeMessageType, PayloadOf, BridgeError } from './protocol';
import type { NativeToWebMessage, RuntimeReadyPayload } from '@sdk';

export interface RequestOptions {
  /**
   * Milliseconds before the request rejects with code "timeout".
   * Default 10000. Pass 0 to disable - required for `settings.captureKey`,
   * which waits on the user pressing a key and legitimately has no deadline.
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
 * The real bridge, reading the global the shared kit decorated.
 *
 * Every member degrades safely when the helper is absent, because this module
 * is also imported by the dev harness before the mock installs, and by unit
 * tests running in plain node.
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

  // Falls back to interpolating the authored English so a view still renders
  // readable text when the helper is missing entirely (plain-browser preview).
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
 * A bridge that is never available - the standalone/plain-browser case, and the
 * default in unit tests. Kept here rather than in the harness so production
 * code can depend on it without pulling dev-only modules into the graph.
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
