// conflicts.ts — key-binding collision bookkeeping (main.legacy.js:1810-1825).
//
// A `settings.changed` push for a `type:"key"` setting carries the CHANGED
// setting's recomputed `conflicts` list, and nothing else. But collisions are
// symmetric: a rebind also changes the PARTNERS' badges — they just gained or
// lost this setting. Rather than re-fetching the whole registry (the old N+1),
// the delta is mirrored onto every other key-typed setting in the local model.
//
// PURITY NOTE: the legacy version MUTATED `mod.schema.groups[].settings[]` in
// place across every loaded mod, relying on the fact that the pane re-reads the
// same object graph on the next render. This port returns a NEW model instead,
// so the port's rendering layer can diff it. Behaviour is otherwise identical,
// quirks included.
//
// `@game` partners (the engine's own bindings) live only INSIDE `conflicts`
// lists — they are not settings and own no badge — so they need no touch-up.

import type { Setting } from '@sdk';
import type { ModRecord } from './rail';
import { titleOf } from './rail';

/** One entry of a `conflicts` list. `mod` may be the reserved id "@game". */
export type ConflictEntry = NonNullable<Setting['conflicts']>[number];

/**
 * Apply a `settings.changed` conflicts delta for `modId`.`settingKey`.
 *
 * Returns a new mods array. Mods, schemas, groups and settings are copied only
 * as deeply as needed to avoid sharing a mutated object with the input.
 */
export function applyConflictUpdate(
  mods: ModRecord[],
  modId: string,
  settingKey: string,
  conflicts: ConflictEntry[],
): ModRecord[] {
  const owner = mods.find((m) => m.id === modId);
  // No such mod: nothing to mirror FROM. The legacy caller reached this
  // function only after finding the mod (main.legacy.js:1775), so this is a
  // guard, not a behaviour.
  if (!owner) return mods;

  const selfEntry: ConflictEntry = { mod: modId, key: settingKey, title: titleOf(owner) };
  // Partners are matched on the "<modId> <key>" pair, joined by a space. A key
  // containing a space could in principle alias another pair here; the native
  // id/key grammars make that unreachable, and this mirrors the legacy join
  // (main.legacy.js:1813) rather than inventing a new encoding.
  const partnered = new Set(conflicts.map((c) => c.mod + ' ' + c.key));

  // The push targets the FIRST setting with this key in this mod — the same one
  // `findSettingInMod` would return (main.legacy.js:297-302, 1785). If a
  // malformed schema declares the key twice, the second copy is neither updated
  // here nor cleaned by the mirror loop below (it is skipped as "self"), so it
  // keeps a stale list. Preserved rather than fixed: the store rejects such
  // schemas upstream, and changing it would alter shipped behaviour.
  let targetApplied = false;

  return mods.map((m) => {
    const groups = m.schema && m.schema.groups;
    if (!groups) return m;

    let modChanged = false;
    const nextGroups = groups.map((g) => {
      const items = g.settings;
      if (!items) return g;

      let groupChanged = false;
      const nextItems = items.map((item) => {
        if (!item || (item as { type?: unknown }).type !== 'key') return item;
        const s = item as Setting;
        const isSelf = m.id === modId && s.key === settingKey;

        if (isSelf) {
          // The changed setting itself: adopt the push list verbatim, and drop
          // the property entirely when it is empty (an absent `conflicts` is
          // the documented "no collisions" encoding — see sdk/osfui.d.ts).
          if (targetApplied) return item;
          targetApplied = true;
          const next = withConflicts(s, conflicts);
          if (next !== s) groupChanged = true;
          return next;
        }

        // Every OTHER key setting: remove any stale entry pointing at the
        // changed setting, then re-add it iff the push names this setting as a
        // partner. Entries pointing at unrelated settings are left alone —
        // this push says nothing about them.
        const existing = Array.isArray(s.conflicts) ? s.conflicts : [];
        const list = existing.filter((c) => !(c && c.mod === modId && c.key === settingKey));
        if (partnered.has(m.id + ' ' + s.key)) list.push(selfEntry);

        const next = withConflicts(s, list);
        if (next !== s) groupChanged = true;
        return next;
      });

      if (!groupChanged) return g;
      modChanged = true;
      return { ...g, settings: nextItems };
    });

    if (!modChanged) return m;
    return { ...m, schema: { ...m.schema, groups: nextGroups } };
  });
}

/**
 * Return `setting` with `conflicts` set to `list`, or with the property REMOVED
 * when the list is empty. Returns the original object unchanged when the result
 * would be structurally identical, so untouched settings keep their identity
 * and a consumer can cheap-diff by reference.
 */
function withConflicts(setting: Setting, list: ConflictEntry[]): Setting {
  const had = Array.isArray(setting.conflicts) ? setting.conflicts : undefined;
  if (list.length === 0) {
    if (had === undefined && !('conflicts' in setting)) return setting;
    const { conflicts: _dropped, ...rest } = setting;
    return rest as Setting;
  }
  if (had && had.length === list.length && had.every((c, i) => c === list[i])) return setting;
  return { ...setting, conflicts: list };
}
