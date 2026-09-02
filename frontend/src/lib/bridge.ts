import type { BridgeError, JsonObject, JsonValue } from '@sdk';

export interface RequestOptions { timeoutMs?: number }

export interface Bridge {
  available(): boolean;
  send(name: string, payload?: JsonObject): boolean;
  request<T extends JsonValue = JsonValue>(
    name: string,
    payload?: JsonObject,
    options?: RequestOptions,
  ): Promise<T>;
  on<T extends JsonValue = JsonValue>(event: string, handler: (payload: T) => void): () => void;
  onAny<T extends JsonValue = JsonValue>(event: string, handler: (payload: T) => void): () => void;
  state<T extends JsonValue = JsonValue>(key: string, handler: (value: T) => void): () => void;
}

function noBridgeError(): BridgeError {
  const error = new Error('no bridge (standalone preview)') as BridgeError;
  error.code = 'no-bridge';
  return error;
}

export const windowBridge: Bridge = {
  available: () => typeof window.osfui?.postMessage === 'function',
  send: (name, payload) => window.osfui?.send(name, payload ?? {}) ?? false,
  request: <T extends JsonValue = JsonValue>(
    name: string,
    payload?: JsonObject,
    options?: RequestOptions,
  ): Promise<T> => {
    if (!window.osfui) return Promise.reject(noBridgeError());
    return window.osfui.request<T>(name, payload ?? {}, options);
  },
  on: <T extends JsonValue = JsonValue>(event: string, handler: (payload: T) => void) =>
    window.osfui?.on<T>(event, handler) ?? (() => {}),
  onAny: <T extends JsonValue = JsonValue>(event: string, handler: (payload: T) => void) =>
    window.osfui?.on<T>(event, handler) ?? (() => {}),
  state: <T extends JsonValue = JsonValue>(key: string, handler: (value: T) => void) =>
    window.osfui?.state.on<T>(key, handler) ?? (() => {}),
};

export const nullBridge: Bridge = {
  available: () => false,
  send: () => false,
  request: () => Promise.reject(noBridgeError()),
  on: () => () => {},
  onAny: () => () => {},
  state: () => () => {},
};

export type * from '@sdk';
