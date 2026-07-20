// Typed envelopes for the native <-> web bridge.
//
// Re-exports `sdk/osfui.d.ts` rather than restating it: that file is the
// published contract (bridge protocol 1.0), kept in lockstep with
// MessageBridge.cpp, docs/authoring-views.md and docs/schema/*.schema.json.
// Importing it turns drift into a compile error.
//
// Everything below the re-export is frontend-only: narrowings the SDK leaves
// loose, plus helpers for parsing untrusted input.

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
 * The error shape `osfui.request()` rejects with. Two guarantees callers rely on:
 *  - `code` is `""`, never `undefined`, when the reply carried no code (`p.code || ""`).
 *  - `reply` is absent on timeout and on the no-bridge rejection — both are
 *    synthesised locally and correspond to no message.
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
 * The command name lands inside the payload, not beside it. Native reads
 * `payload.command`, so that is contract, not an accident — do not "tidy" it.
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
  // Native treats an absent id as fire-and-forget. Attaching only when present
  // keeps the emitted JSON byte-identical to the shipped helper's.
  if (requestId !== undefined) envelope.requestId = requestId;
  return envelope;
}

/**
 * Parse a native->web frame, matching the shipped helper's tolerance: malformed
 * JSON and a non-string `type` return `null` rather than throwing, so one bad
 * frame on the one-way firehose cannot kill the view.
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
    // Subscribers rely on a missing payload arriving as {}.
    payload: m.payload ?? {},
    ...(typeof m.requestId === 'string' ? { requestId: m.requestId } : {}),
  };
}

/**
 * The helper rejects on `ui.error` and on `ui.result` with `ok: false`, and on
 * nothing else; `ui.result` with `ok: true` and any typed reply resolve.
 */
export function isFailureReply(message: BridgeEnvelope): boolean {
  if (message.type === 'ui.error') return true;
  if (message.type !== 'ui.result') return false;
  const p = message.payload as { ok?: unknown } | null;
  return !!p && p.ok === false;
}
