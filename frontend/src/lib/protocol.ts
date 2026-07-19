// protocol.ts — typed envelopes for the native <-> web bridge.
//
// This module deliberately RE-EXPORTS `sdk/osfui.d.ts` instead of restating it.
// That file is the published contract (bridge protocol 1.0) and is already kept
// in lockstep with src/runtime/MessageBridge.cpp, docs/authoring-views.md and
// docs/schema/*.schema.json. Mirroring it here by hand would create a fourth
// copy to keep in sync; importing it makes drift a compile error instead.
//
// Everything below this re-export is FRONTEND-ONLY refinement: narrowings the
// SDK intentionally leaves loose, and helpers for parsing untrusted input.

export type * from '@sdk';

import type {
  BridgeEnvelope,
  NativeToWebMessage,
  UiCommand,
  UiCommandAction,
} from '@sdk';

/** Every `type` the runtime can send us, derived from the SDK union. */
export type NativeMessageType = NativeToWebMessage['type'];

/** Narrow a message union member by its `type` tag. */
export type MessageOf<T extends NativeMessageType> = Extract<NativeToWebMessage, { type: T }>;

/** Payload of a given message type. */
export type PayloadOf<T extends NativeMessageType> = MessageOf<T>['payload'];

/** Anything acceptable as an outbound command, including mod-registered actions. */
export type AnyCommand = UiCommand | UiCommandAction;

/**
 * The error shape `osfui.request()` rejects with.
 *
 * Two details the shipped helper guarantees and callers rely on:
 *  - `code` is `""` (never `undefined`) when the reply carried no code, because
 *    it is assigned `p.code || ""`.
 *  - `reply` is ABSENT on timeout and on the no-bridge rejection - those are
 *    synthesised locally and never correspond to a message.
 */
export interface BridgeError extends Error {
  code: string;
  reply?: BridgeEnvelope;
}

export function isBridgeError(e: unknown): e is BridgeError {
  return e instanceof Error && typeof (e as BridgeError).code === 'string';
}

/**
 * Build the outbound envelope exactly as `osfui.send`/`osfui.request` do.
 *
 * Note the shape faithfully reproduces one quirk of the shipped helper: the
 * command name lands INSIDE the payload (`Object.assign({ command }, fields)`),
 * not beside it. Native reads `payload.command`, so this is contract, not an
 * accident - do not "tidy" it.
 */
export function encodeCommand(
  command: string,
  fields?: Record<string, unknown>,
  requestId?: string,
): BridgeEnvelope<'ui.command', Record<string, unknown>> {
  const envelope: BridgeEnvelope<'ui.command', Record<string, unknown>> = {
    type: 'ui.command',
    payload: Object.assign({ command }, fields || {}),
  };
  // Only attach when present: native treats an absent id as fire-and-forget,
  // and an explicit `undefined` would serialise the key away anyway - but being
  // explicit keeps the emitted JSON byte-identical to the shipped helper's.
  if (requestId !== undefined) envelope.requestId = requestId;
  return envelope;
}

/**
 * Parse a native->web frame. Mirrors the shipped helper's tolerance exactly:
 * malformed JSON and non-string `type` are IGNORED (not thrown), because the
 * bridge is a one-way firehose and one bad frame must not kill the view.
 *
 * Returns `null` for anything unusable.
 */
export function parseMessage(json: string): BridgeEnvelope | null {
  let message: unknown;
  try {
    message = JSON.parse(json);
  } catch {
    return null;
  }
  if (!message || typeof message !== 'object') return null;
  const m = message as Partial<BridgeEnvelope>;
  if (typeof m.type !== 'string') return null;
  return {
    type: m.type,
    // The helper coerces a missing payload to {} before dispatch; subscribers
    // are written against that guarantee.
    payload: m.payload ?? {},
    ...(typeof m.requestId === 'string' ? { requestId: m.requestId } : {}),
  };
}

/**
 * Does this reply mean "the request failed"? The helper rejects on `ui.error`
 * and on `ui.result` carrying `ok: false` - and ONLY on those. A `ui.result`
 * with `ok: true`, or any typed reply, resolves.
 */
export function isFailureReply(message: BridgeEnvelope): boolean {
  if (message.type === 'ui.error') return true;
  if (message.type !== 'ui.result') return false;
  const p = message.payload as { ok?: unknown } | null;
  return !!p && p.ok === false;
}
