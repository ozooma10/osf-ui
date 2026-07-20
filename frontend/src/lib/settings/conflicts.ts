// conflicts.ts — key-binding collision bookkeeping.
//
// A `settings.changed` push for a `type:"key"` setting carries only that
// setting's recomputed `conflicts` list. Collisions are symmetric, so a rebind
// also changes the partners' badges — they just gained or lost this setting.
// Rather than re-fetch the whole registry, the delta is mirrored onto every
// other key-typed setting in the local model. Returns a new model so the
// rendering layer can diff it.
//
// `@game` partners (the engine's own bindings) exist only inside `conflicts`
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
  // Guard only — callers reach here having already found the mod.
  if (!owner) return mods;

  const selfEntry: ConflictEntry = { mod: modId, key: settingKey, title: titleOf(owner) };
  // Partners are matched on the "<modId> <key>" pair joined by a space. A key
  // containing a space could alias another pair; the native id/key grammars
  // make that unreachable.
  const partnered = new Set(conflicts.map((c) => c.mod + ' ' + c.key));

  // The push targets the first setting with this key in this mod, matching
  // `findSettingInMod`. If a malformed schema declares the key twice, the
  // second copy is neither updated here nor cleaned by the mirror loop below
  // (skipped as "self"), so it keeps a stale list. Left as-is: the store
  // rejects such schemas upstream, and changing it would alter shipped
  // behaviour.
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
          // Adopt the push list verbatim, dropping the property entirely when
          // empty: an absent `conflicts` is the documented "no collisions"
          // encoding (sdk/osfui.d.ts).
          if (targetApplied) return item;
          targetApplied = true;
          const next = withConflicts(s, conflicts);
          if (next !== s) groupChanged = true;
          return next;
        }

        // Every other key setting: drop any stale entry pointing at the changed
        // setting, re-add it iff the push names this setting as a partner.
        // Entries pointing at unrelated settings are left alone — this push
        // says nothing about them.
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
 * Return `setting` with `conflicts` set to `list`, or with the property removed
 * when the list is empty. Returns the original object when the result would be
 * structurally identical, so untouched settings keep their identity and
 * consumers can diff by reference.
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
