
import type { Setting } from '@sdk';
import type { ModRecord } from './rail';
import { titleOf } from './rail';

/** One entry of a `conflicts` list. `mod` may be the reserved id "@game". */
export type ConflictEntry = NonNullable<Setting['conflicts']>[number];

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
  const partnered = new Set(conflicts.map((c) => c.mod + ' ' + c.key));

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
          if (targetApplied) return item;
          targetApplied = true;
          const next = withConflicts(s, conflicts);
          if (next !== s) groupChanged = true;
          return next;
        }

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
