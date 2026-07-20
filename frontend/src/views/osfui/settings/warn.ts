// The view's schema-diagnostic channel: messages aimed at a mod author reading
// OSF UI.log after a schema misbehaves (condition naming a missing key, setting
// with no key, step of zero). None is user-facing — the pane still renders,
// degraded.
//
// Not wrapped in `import.meta.env.DEV`: these must fire against third-party
// schemas in shipped builds on end-user machines, or "my setting doesn't show
// up" is undiagnosable in the field.

/** Stable log prefix — existing log-grep habits depend on it. */
const PREFIX = '[osfui settings] ';

export function devWarn(message: string): void {
  // A missing `console.warn` must not throw out of a render.
  if (typeof console !== 'undefined' && console.warn) console.warn(PREFIX + message);
}
