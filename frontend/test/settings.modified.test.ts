import { describe, it, expect } from 'vitest';
import type { Setting } from '@sdk';
import type { ModRecord } from '@lib/settings/rail';
import type { Baseline } from '@lib/settings/modified';
import {
  findSettingInMod,
  isModified,
  modifiedCount,
  sameValue,
  sessionChangeCount,
  sessionDiff,
} from '@lib/settings/modified';

const setting = (o: Partial<Setting> & Pick<Setting, 'key' | 'type'>): Setting => o as Setting;

const mod = (over: Partial<ModRecord> = {}): ModRecord => ({
  id: 'acme.demo',
  title: 'Demo',
  schema: {
    groups: [
      {
        label: 'Main',
        settings: [
          setting({ key: 'on', type: 'bool', default: true }),
          setting({ key: 'level', type: 'int', default: 5 }),
          setting({ key: 'tags', type: 'flags', options: ['a', 'b'], default: ['a'] }),
          setting({ key: 'nodefault', type: 'string' }),
          { type: 'note', text: 'not a setting' },
        ],
      },
    ],
  },
  values: { on: true, level: 5, tags: ['a'] },
  ...over,
});

describe('sameValue — structural for objects', () => {
  it('compares arrays structurally (`!==` is always true for two arrays)', () => {
    expect(sameValue(['a', 'b'], ['a', 'b'])).toBe(true);
    expect(sameValue(['a', 'b'], ['b', 'a'])).toBe(false);
    expect(sameValue([], [])).toBe(true);
  });

  it('is order-sensitive on purpose (the store canonicalises flags order)', () => {
    expect(sameValue(['a', 'b'], ['b', 'a'])).toBe(false);
  });

  it('goes structural when only ONE side is an object', () => {
    expect(sameValue([], 0)).toBe(false);
    expect(sameValue([], '[]')).toBe(false);
  });

  it('compares scalars by identity', () => {
    expect(sameValue(5, 5)).toBe(true);
    expect(sameValue(5, '5')).toBe(false);
    expect(sameValue(true, true)).toBe(true);
    expect(sameValue(undefined, undefined)).toBe(true);
  });
});

describe('isModified', () => {
  it('is false when the value is undefined', () => {
    expect(isModified(setting({ key: 'k', type: 'int', default: 1 }), undefined)).toBe(false);
  });

  it('is false when the schema declares NO default key at all', () => {
    expect(isModified(setting({ key: 'k', type: 'string' }), 'anything')).toBe(false);
  });

  it('compares scalars against the default', () => {
    const s = setting({ key: 'k', type: 'int', default: 5 });
    expect(isModified(s, 5)).toBe(false);
    expect(isModified(s, 6)).toBe(true);
  });

  it('compares FLAGS structurally, so an equal array is NOT modified', () => {
    const s = setting({ key: 'k', type: 'flags', options: ['a', 'b'], default: ['a', 'b'] });
    expect(isModified(s, ['a', 'b'])).toBe(false);
    expect(isModified(s, ['b', 'a'])).toBe(true);
    expect(isModified(s, [])).toBe(true);
  });

  it('goes structural when only the default is an object', () => {
    const s = setting({ key: 'k', type: 'flags', options: ['a'], default: [] });
    expect(isModified(s, 'a')).toBe(true);
  });

  it('an explicit `default: undefined` is still a declared key, but the value guard wins', () => {
    const s = { key: 'k', type: 'int', default: undefined } as unknown as Setting;
    expect('default' in s).toBe(true);
    expect(isModified(s, undefined)).toBe(false);
    expect(isModified(s, 1)).toBe(true);
  });
});

describe('findSettingInMod', () => {
  it('finds a setting by key', () => {
    expect(findSettingInMod(mod(), 'level')?.type).toBe('int');
  });
  it('returns null for an unknown key and for a mod with no schema', () => {
    expect(findSettingInMod(mod(), 'nope')).toBeNull();
    expect(findSettingInMod({ id: 'x' }, 'k')).toBeNull();
  });
  it('is NOT gated on isSetting — a keyed non-setting item is returned', () => {
    const m: ModRecord = {
      id: 'x',
      schema: { groups: [{ settings: [{ type: 'action', key: 'run', label: 'Run', command: 'x.run' }] }] },
    };
    expect(findSettingInMod(m, 'run')?.type).toBe('action' as never);
  });
});

describe('modifiedCount', () => {
  it('counts nothing when every value equals its default', () => {
    expect(modifiedCount(mod())).toBe(0);
  });

  it('counts scalar and flags divergences, ignoring notes', () => {
    expect(modifiedCount(mod({ values: { on: false, level: 5, tags: ['a', 'b'] } }))).toBe(2);
  });

  it('ignores keys with no stored value and settings with no default', () => {
    expect(modifiedCount(mod({ values: { nodefault: 'typed something' } }))).toBe(0);
  });

  it('is 0 for a mod with no schema or no values', () => {
    expect(modifiedCount({ id: 'x' })).toBe(0);
    expect(modifiedCount(mod({ values: {} }))).toBe(0);
  });
});

describe('sessionDiff / sessionChangeCount', () => {
  it('reports only keys that actually differ from the baseline', () => {
    const mods = [mod({ values: { on: false, level: 5, tags: ['a'] } })];
    const baseline: Baseline = { 'acme.demo': { on: true, level: 5, tags: ['a'] } };
    const changes = sessionDiff(baseline, mods);
    expect(changes.map((c) => c.key)).toEqual(['on']);
    expect(changes[0]).toMatchObject({ modId: 'acme.demo', old: true, now: false });
    expect(sessionChangeCount(baseline, mods)).toBe(1);
  });

  it('does NOT report a flags array that is structurally unchanged', () => {
    const mods = [mod({ values: { on: true, level: 5, tags: ['a'] } })];
    // A fresh array with the same contents — `!==` would have reported it.
    const baseline: Baseline = { 'acme.demo': { tags: ['a'] } };
    expect(sessionDiff(baseline, mods)).toEqual([]);
  });

  it('reports a key whose baseline was undefined and now has a value', () => {
    const mods = [mod({ values: { on: true, level: 5, tags: ['a'], nodefault: 'x' } })];
    const baseline: Baseline = { 'acme.demo': { nodefault: undefined } };
    expect(sessionDiff(baseline, mods).map((c) => c.key)).toEqual(['nodefault']);
  });

  it('SKIPS a baseline entry whose mod is no longer loaded', () => {
    const baseline: Baseline = { 'gone.mod': { k: 1 } };
    expect(sessionDiff(baseline, [mod()])).toEqual([]);
    expect(sessionChangeCount(baseline, [mod()])).toBe(0);
  });

  it('carries the mod record for the revert panel caption', () => {
    const m = mod({ values: { on: false, level: 5, tags: ['a'] } });
    const changes = sessionDiff({ 'acme.demo': { on: true } }, [m]);
    expect(changes[0]?.mod).toBe(m);
  });
});
