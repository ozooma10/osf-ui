// Validation for the harness's manual native -> web envelope editor.
// Keep this separate from shell.js so the protocol boundary is unit-testable
// without a browser DOM.

export const NATIVE_TO_WEB_KINDS = new Set(['ready', 'state', 'event', 'reply', 'error']);

export function parseNativeEnvelope(source) {
  const message = JSON.parse(source);
  if (!message || typeof message !== 'object' || Array.isArray(message) ||
      !NATIVE_TO_WEB_KINDS.has(message.kind)) {
    throw new Error(
      'message.kind must be one of: ready, state, event, reply, error',
    );
  }
  return message;
}
