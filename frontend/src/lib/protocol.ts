// Typed envelopes for the native <-> web bridge.
//
// Re-exports `sdk/osfui.d.ts` rather than restating it: that file is the
// published contract (bridge protocol 2.0), kept in lockstep with
// MessageBridge.cpp, docs/authoring-views.md and docs/schema/*.schema.json.
// Importing it turns drift into a compile error.
//
// The declarations below add frontend narrowings the published SDK leaves loose.

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

/**
 * The error shape `osfui.request()` rejects with. Two guarantees callers rely on:
 *  - `code` is `""`, never `undefined`, when the reply carried no code.
 *  - `payload` is absent on timeout and on the no-bridge rejection — both are
 *    synthesised locally and correspond to no message.
 */
export interface BridgeError extends Error {
  code: string;
  payload?: { code: string; message: string };
}

/**
 * The machine `code` off a rejected `osfui.request()`, or `""`.
 *
 * Deliberately structural rather than `isBridgeError`-gated: a reject value is
 * not guaranteed to be an `Error` (a thrown handler, a non-Error reject), and
 * every caller only wants to compare the code against a known string. Upholds
 * {@link BridgeError}'s `""`-never-`undefined` guarantee for those comparisons.
 */
export function codeOf(err: unknown): string {
  const e = err as { code?: unknown } | null;
  return e && typeof e.code === 'string' ? e.code : '';
}
