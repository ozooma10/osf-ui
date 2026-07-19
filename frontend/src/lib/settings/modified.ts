// modified.ts — "is this different from the default?" and "what have I changed
// since I opened settings?" (main.legacy.js:303-337, 1491-1501).
//
// Both questions have the same trap: a `flags` value is an ARRAY, so `!==` is
// ALWAYS true for two structurally identical values and every flags setting
// would show a permanent modified dot and a permanent undo entry. Hence the
// JSON.stringify comparison below — see `sameValue`.

import type { Setting, SettingValue } from '@sdk';
import type { ModRecord } from './rail';
import { isSetting } from './normalize';

/**
 * Structural equality for setting values (main.legacy.js:322-325).
 *
 * `JSON.stringify` is used, not a deep-equal: setting values are the closed
 * `SettingValue` set (boolean | number | string | string[]), so stringify is
 * total, order-sensitive (which is CORRECT — the store canonicalises flags to
 * declared order, so a different order means a different stored value), and has
 * no cycles to worry about.
 *
 * The `typeof === "object"` guard means the stringify path is taken when EITHER
 * side is an object, so comparing an array against a scalar still goes
 * structural ("[]" vs "0") rather than reference-equal.
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
 *  - `value === undefined` — the mod has no stored value for this key yet, so
 *    there is nothing to have changed.
 *  - no `default` key in the schema — a setting that declares no default has
 *    nothing to be modified FROM. Note this is a key-presence test, so an
 *    explicit `default: undefined` still counts as declared and the comparison
 *    runs (yielding "not modified" via the undefined check above anyway).
 */
export function isModified(setting: Setting, value: SettingValue | undefined): boolean {
  if (value === undefined || !('default' in setting)) return false;
  if (typeof value === 'object' || typeof setting.default === 'object') {
    return JSON.stringify(value) !== JSON.stringify(setting.default);
  }
  return value !== setting.default;
}

/** The schema setting object for a mod's key, or null. main.legacy.js:297-302. */
export function findSettingInMod(mod: ModRecord, key: string): Setting | null {
  for (const g of (mod.schema && mod.schema.groups) || []) {
    for (const s of g.settings || []) {
      // Deliberately NOT gated on isSetting: the legacy lookup matches any item
      // carrying this key, which is how a keyed `action` item could be returned.
      // Callers check `.type` themselves (main.legacy.js:1786).
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
 * The session baseline: `baseline[modId][key]` = the value when this VISIT
 * began. Nested rather than a joined "mod key" string so a key containing a
 * space cannot corrupt the split (main.legacy.js:85).
 */
export type Baseline = Record<string, Record<string, SettingValue | undefined>>;

export interface SessionChange {
  modId: string;
  key: string;
  /** The value when the visit began (may be undefined — key had no value). */
  old: SettingValue | undefined;
  now: SettingValue | undefined;
  mod: ModRecord;
}

/**
 * Everything changed since the baseline was taken — the undo chip's count and
 * the revert panel's list, from one function (main.legacy.js:326-337 and
 * 1491-1501 were two copies of this loop).
 *
 * A baseline entry whose mod is no longer loaded is SKIPPED, not reported: a
 * mod that unregistered mid-visit has nothing to revert into.
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

/** The undo chip's number. */
export function sessionChangeCount(baseline: Baseline, mods: ModRecord[]): number {
  return sessionDiff(baseline, mods).length;
}
