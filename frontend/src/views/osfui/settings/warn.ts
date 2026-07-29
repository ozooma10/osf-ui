// The view's schema-diagnostic channel: messages aimed at a mod author reading
// OSF UI.log after a schema misbehaves (condition naming a missing key, setting
// with no key, step of zero). None is user-facing — the pane still renders,
// degraded.
//
// Not wrapped in `import.meta.env.DEV`: these must fire against third-party
// schemas in shipped builds on end-user machines, or "my setting doesn't show
// up" is undiagnosable in the field.
//
// Every caller sits in a RENDER path — `evalGate`'s unknown-key reporter runs
// per gated item, `hasInvalidStep` per numeric row — and the pane re-renders on
// every settings.changed push, every filter-debounce tick and every preset
// apply. Emitted verbatim, one bad condition in one schema would write a line
// to OSF UI.log for the whole visit, so each distinct message is logged once per
// page load. That is the right granularity: the message text already names the
// mod, the key and the fault, so a repeat carries no information a reader of
// the log does not already have.

/** Stable log prefix — existing log-grep habits depend on it. */
const PREFIX = '[osfui settings] ';

/**
 * Messages already logged this page load. Unbounded by construction, and safely
 * so: the key set is bounded by the schemas loaded at startup, so it cannot grow
 * with time or with user activity.
 */
const seen = new Set<string>();

export function devWarn(message: string): void {
  if (seen.has(message)) return;
  seen.add(message);
  // A missing `console.warn` must not throw out of a render.
  if (typeof console !== 'undefined' && console.warn) console.warn(PREFIX + message);
}

/** Test seam: forget what has been logged, so cases do not leak into each other. */
export function resetDevWarnDedupe(): void {
  seen.clear();
}
