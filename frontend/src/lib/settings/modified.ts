// "Is this different from the default?" and "what have I changed since I opened
// settings?"
//
// Both share one trap: a `flags` value is an array, so `!==` is always true for
// two structurally identical values and every flags setting would show a
// permanent modified dot and a permanent undo entry. Hence `sameValue`.

import type { Setting, SettingValue } from '@sdk';
import type { ModRecord } from './rail';
import { isSetting } from './normalize';

/**
 * Structural equality for setting values.
 *
 * `JSON.stringify` rather than a deep-equal: values are the closed
 * `SettingValue` set (boolean | number | string | string[]), so stringify is
 * total and cycle-free. Order-sensitivity is wanted — the store canonicalises
 * flags to declared order, so a different order is a different stored value.
 *
 * The `typeof === "object"` guard takes the stringify path when either side is
 * an object, so array-vs-scalar still compares structurally ("[]" vs "0").
 */
export function sameValue(a: unknown, b: unknown): boolean {
  if (typeof a === 'object' || typeof b === 'object') {
    return JSON.stringify(a) === JSON.stringify(b);
  }
  return a === b;
}

/**
 * Is `value` different from the setting's declared default?
 *
 * Two "not modified" short-circuits, both load-bearing:
 *  - `value === undefined`: no stored value for this key yet, nothing to change.
 *  - no `default` key in the schema: nothing to be modified from. Key-presence
 *    test, so an explicit `default: undefined` counts as declared and the
 *    comparison runs.
 */
export function isModified(setting: Setting, value: SettingValue | undefined): boolean {
  if (value === undefined || !('default' in setting)) return false;
  // sameValue already encapsulates the flags-array trap (see its doc).
  return !sameValue(value, setting.default);
}

/** The schema setting object for a mod's key, or null. */
export function findSettingInMod(mod: ModRecord, key: string): Setting | null {
  for (const g of (mod.schema && mod.schema.groups) || []) {
    for (const s of g.settings || []) {
      // Not gated on isSetting: matches any item carrying this key, so a keyed
      // `action` item can come back. Callers check `.type` themselves.
      if (s && (s as { key?: unknown }).key === key) return s as Setting;
    }
  }
  return null;
}

/** How many of a mod's settings differ from their defaults (the rail badge). */
export function modifiedCount(mod: ModRecord): number {
  let n = 0;
  const values = mod.values || {};
  for (const g of (mod.schema && mod.schema.groups) || []) {
    for (const s of g.settings || []) {
      if (isSetting(s) && isModified(s, values[s.key])) n++;
    }
  }
  return n;
}

/**
 * Session baseline: `baseline[modId][key]` = the value when this visit began.
 * Nested rather than a joined "mod key" string so a key containing a space
 * cannot corrupt the split.
 */
export type Baseline = Record<string, Record<string, SettingValue | undefined>>;

/**
 * Merge `patch` into one mod's values, returning a new list. Rows other than
 * `modId` come back by identity, so unrelated entries keep their references.
 */
export function patchModValues(
  list: ModRecord[],
  modId: string,
  patch: Record<string, SettingValue>,
): ModRecord[] {
  return list.map((m) => (m.id === modId ? { ...m, values: { ...(m.values || {}), ...patch } } : m));
}

/**
 * Record the pre-change value of every key in `keys` that is not tracked yet,
 * returning the new baseline — or null when nothing needed seeding, so the
 * caller can skip a render.
 *
 * Seeded once per key: the first change is what the visit is measured from, and
 * a second edit of the same key must not move the goalposts.
 *
 * `ensureEntry` additionally creates an entry for a mod that has none, which
 * records "this mod has been snapshotted". ONLY the whole-list capture wants
 * that — the per-change callers must not, or applying a preset that touches no
 * keys would seed an entry and force a pointless render.
 */
export function seedBaseline(
  base: Baseline,
  modId: string,
  keys: Iterable<string>,
  values: Record<string, SettingValue | undefined>,
  ensureEntry = false,
): Baseline | null {
  const tracked = { ...(base[modId] || {}) };
  let changed = false;
  for (const key of keys) {
    if (!(key in tracked)) {
      tracked[key] = values[key];
      changed = true;
    }
  }
  if (!changed && !(ensureEntry && !base[modId])) return null;
  return { ...base, [modId]: tracked };
}

export interface SessionChange {
  modId: string;
  key: string;
  /** The value when the visit began (may be undefined — key had no value). */
  old: SettingValue | undefined;
  now: SettingValue | undefined;
  mod: ModRecord;
}

/**
 * Everything changed since the baseline was taken — feeds both the undo chip's
 * count (as `.length`; the App needs the list anyway to render the panel, so
 * there is no count-only entry point) and the revert panel's list.
 *
 * A baseline entry whose mod is no longer loaded is skipped, not reported: a mod
 * that unregistered mid-visit has nothing to revert into.
 */
export function sessionDiff(baseline: Baseline, mods: ModRecord[]): SessionChange[] {
  const changes: SessionChange[] = [];
  for (const modId in baseline) {
    const mod = mods.find((m) => m.id === modId);
    if (!mod) continue;
    const values = mod.values || {};
    const tracked = baseline[modId] || {};
    for (const key in tracked) {
      const old = tracked[key];
      const now = values[key];
      if (!sameValue(now, old)) changes.push({ modId, key, old, now, mod });
    }
  }
  return changes;
}
