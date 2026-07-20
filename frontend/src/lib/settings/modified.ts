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
  if (typeof value === 'object' || typeof setting.default === 'object') {
    return JSON.stringify(value) !== JSON.stringify(setting.default);
  }
  return value !== setting.default;
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
 * count and the revert panel's list.
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

/** The undo chip's number. */
export function sessionChangeCount(baseline: Baseline, mods: ModRecord[]): number {
  return sessionDiff(baseline, mods).length;
}
