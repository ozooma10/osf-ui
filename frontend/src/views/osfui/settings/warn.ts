// warn.ts — the view's schema-diagnostic channel.
//
// Ports `devWarn` (settings/main.legacy.js:216). Every message it carries is
// aimed at a MOD AUTHOR looking at OSF UI.log with a schema that does not do
// what they expected — a condition naming a key that does not exist, a setting
// with no key, a step of zero. None of them is a user-facing error: the pane
// still renders, degraded, which is the whole point.
//
// It is NOT wrapped in `import.meta.env.DEV`, deliberately. These fire against
// third-party schemas in a shipped build, on an end user's machine, and that is
// exactly where a mod author needs them to have fired — Ultralight's console
// output lands in OSF UI.log. Stripping them would make "my setting doesn't
// show up" undiagnosable in the field.

/** The log prefix legacy used; kept so existing log-grep habits still work. */
const PREFIX = '[osfui settings] ';

export function devWarn(message: string): void {
  // Guarded the way legacy guarded it: Ultralight's console has historically
  // been partial, and a missing `console.warn` must not throw out of a render.
  if (typeof console !== 'undefined' && console.warn) console.warn(PREFIX + message);
}
