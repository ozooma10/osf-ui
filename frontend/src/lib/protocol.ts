
export type * from '@sdk';

import type { PlatformEvents, PlatformState } from '@sdk';

/** Every event name the runtime can raise at us. */
export type EventName = keyof PlatformEvents;

/** Payload of a given event. */
export type EventPayload<T extends EventName> = PlatformEvents[T];

/** Every platform state key, in its absolute `<mod>/<key>` spelling. */
export type StateKey = keyof PlatformState;

/** Value carried by a given state key. */
export type StateValue<T extends StateKey> = PlatformState[T];

export interface BridgeError extends Error {
  code: string;
  payload?: { code: string; message: string };
}

export function codeOf(err: unknown): string {
  const e = err as { code?: unknown } | null;
  return e && typeof e.code === 'string' ? e.code : '';
}
