
/** Stable log prefix — existing log-grep habits depend on it. */
const PREFIX = '[osfui settings] ';

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
