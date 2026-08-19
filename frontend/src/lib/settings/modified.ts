
import type { Setting, SettingValue } from '@sdk';
import type { ModRecord } from './rail';
import { isSetting } from './normalize';

export function sameValue(a: unknown, b: unknown): boolean {
  if (typeof a === 'object' || typeof b === 'object') {
    return JSON.stringify(a) === JSON.stringify(b);
  }
  return a === b;
}

export function isModified(setting: Setting, value: SettingValue | undefined): boolean {
  if (value === undefined || !('default' in setting)) return false;
  // sameValue already encapsulates the flags-array trap (see its doc).
  return !sameValue(value, setting.default);
}

/** The schema setting object for a mod's key, or null. */
export function findSettingInMod(mod: ModRecord, key: string): Setting | null {
  for (const g of (mod.schema && mod.schema.groups) || []) {
    for (const s of g.settings || []) {
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

export type Baseline = Record<string, Record<string, SettingValue | undefined>>;

export function patchModValues(
  list: ModRecord[],
  modId: string,
  patch: Record<string, SettingValue>,
): ModRecord[] {
  return list.map((m) => (m.id === modId ? { ...m, values: { ...(m.values || {}), ...patch } } : m));
}

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
