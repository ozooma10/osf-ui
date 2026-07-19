// inputContext.ts — resolving a `type:"key"` setting's input context
// (main.legacy.js:187-209).
//
// A key binding may declare that it is only live inside a mod-local "input
// context". `gameplay` is the implicit default and is RESERVED: it can never be
// redeclared, so a schema cannot relabel or re-flag it. Everything here is
// display metadata — the badge next to a key row — and the runtime's dispatch
// is unaffected either way.

import type { InputContext, Setting, SettingsSchema } from '@sdk';

/**
 * The id grammar, mirrored from the native side (SettingsStore.cpp
 * `kMaxInputContextIdLen` = 64): an alphanumeric first character, then up to 63
 * more of [A-Za-z0-9._-]. Anchored, so a newline-bearing id cannot slip past.
 * main.legacy.js:187.
 */
export const INPUT_CONTEXT_ID_RE = /^[A-Za-z0-9][A-Za-z0-9._-]{0,63}$/;

/** The reserved id of the implicit default context. */
export const GAMEPLAY_ID = 'gameplay';

/** A resolved context, always fully populated (no optional fields to defend). */
export interface ResolvedInputContext {
  id: string;
  label: string;
  blocksGameplay: boolean;
}

/**
 * Build the implicit gameplay fallback. `label` is injected so this module
 * stays free of the localiser (the legacy code called `tr("gameplay",
 * "Gameplay")` inline at main.legacy.js:189).
 */
export function gameplayContext(label = 'Gameplay'): ResolvedInputContext {
  return { id: GAMEPLAY_ID, label, blocksGameplay: false };
}

/**
 * The schema's declared contexts, filtered and deduped exactly as the legacy
 * lookup does on the fly (main.legacy.js:194-199):
 *
 *  - non-object entries are dropped;
 *  - an entry redeclaring the reserved `gameplay` id is dropped;
 *  - an entry whose id fails the grammar is dropped;
 *  - on a DUPLICATE id, the FIRST declaration wins and later ones are dropped.
 *
 * Note the dedupe records an id as seen BEFORE the match test, so first-wins
 * holds even when a later duplicate is the one being searched for.
 */
export function dedupeInputContexts(contexts: unknown): ResolvedInputContext[] {
  if (!Array.isArray(contexts)) return [];
  const seen = new Set<string>();
  const out: ResolvedInputContext[] = [];
  for (const raw of contexts) {
    if (!raw || typeof raw !== 'object') continue;
    const c = raw as Partial<InputContext>;
    const id = typeof c.id === 'string' ? c.id : '';
    if (id === GAMEPLAY_ID || !INPUT_CONTEXT_ID_RE.test(id) || seen.has(id)) continue;
    seen.add(id);
    out.push({
      id,
      // `typeof === "string" && label` — an empty label falls back to the id, so
      // a badge is never blank.
      label: typeof c.label === 'string' && c.label ? c.label : id,
      // Strict `=== true`: any other truthy value is NOT an assertion.
      blocksGameplay: c.blocksGameplay === true,
    });
  }
  return out;
}

/**
 * Resolve the context a key setting belongs to.
 *
 * FOUR paths fall back to the implicit gameplay context, and they are distinct
 * cases even though they share a result:
 *  1. no `inputContext` on the setting (or a non-string one) — the normal case;
 *  2. an explicit `inputContext: "gameplay"` — the reserved id resolves to the
 *     implicit context WITHOUT consulting the schema, so it can never pick up a
 *     rogue declaration's label or `blocksGameplay`;
 *  3. an `inputContext` that fails `INPUT_CONTEXT_ID_RE`;
 *  4. a VALID id that no surviving `schema.inputContexts` entry declares — the
 *     dangling-reference case (a context removed from the schema, or shadowed
 *     out by the dedupe above). The setting still works; it just loses its
 *     badge.
 *
 * Case 4 is why the badge is suppressed whenever `id === "gameplay"`: an
 * unresolvable reference must read as "no special context", not as a broken
 * badge (main.legacy.js:575-582).
 */
export function resolveInputContext(
  schema: SettingsSchema | undefined,
  setting: Pick<Setting, 'inputContext'> | undefined,
  gameplayLabel = 'Gameplay',
): ResolvedInputContext {
  const fallback = gameplayContext(gameplayLabel);
  const ref = setting && typeof setting.inputContext === 'string' ? setting.inputContext : '';
  // Cases 1-3.
  if (!ref || ref === GAMEPLAY_ID || !INPUT_CONTEXT_ID_RE.test(ref)) return fallback;
  const declared = dedupeInputContexts(schema && schema.inputContexts);
  // Case 4 when nothing matches.
  return declared.find((c) => c.id === ref) || fallback;
}
