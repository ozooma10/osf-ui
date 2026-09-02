import type { BridgeError as SDKBridgeError, JsonValue } from '@sdk';

export type EventName = string;
export type EventPayload<T extends EventName> = T extends string ? JsonValue : never;
export type StateKey = string;
export type StateValue<T extends StateKey> = T extends string ? JsonValue : never;
export type BridgeError = SDKBridgeError;

export function codeOf(error: unknown): string {
  const candidate = error as { code?: unknown } | null;
  return candidate && typeof candidate.code === 'string' ? candidate.code : '';
}
