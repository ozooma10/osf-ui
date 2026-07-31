// Typed envelopes for the native <-> web bridge.
//
// Re-exports `sdk/osfui.d.ts` rather than restating it: that file is the
// published contract (bridge protocol 1.0), kept in lockstep with
// MessageBridge.cpp, docs/authoring-views.md and docs/schema/*.schema.json.
// Importing it turns drift into a compile error.
//
// The declarations below add frontend narrowings the published SDK leaves loose.

export type * from '@sdk';

import type { BridgeEnvelope, NativeToWebMessage } from '@sdk';

/** Every `type` the runtime can send us, derived from the SDK union. */
export type NativeMessageType = NativeToWebMessage['type'];

/** Narrow a message union member by its `type` tag. */
export type MessageOf<T extends NativeMessageType> = Extract<NativeToWebMessage, { type: T }>;

/** Payload of a given message type. */
export type PayloadOf<T extends NativeMessageType> = MessageOf<T>['payload'];

/**
 * The error shape `osfui.request()` rejects with. Two guarantees callers rely on:
 *  - `code` is `""`, never `undefined`, when the reply carried no code (`p.code || ""`).
 *  - `reply` is absent on timeout and on the no-bridge rejection — both are
 *    synthesised locally and correspond to no message.
 */
export interface BridgeError extends Error {
  code: string;
  reply?: BridgeEnvelope;
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
