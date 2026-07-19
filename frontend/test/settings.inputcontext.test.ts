import { describe, it, expect } from 'vitest';
import type { SettingsSchema } from '@sdk';
import {
  GAMEPLAY_ID,
  INPUT_CONTEXT_ID_RE,
  dedupeInputContexts,
  gameplayContext,
  resolveInputContext,
} from '@lib/settings/inputContext';

const schema = (inputContexts: unknown): SettingsSchema =>
  ({ inputContexts }) as SettingsSchema;

describe('INPUT_CONTEXT_ID_RE', () => {
  it('accepts an alphanumeric first char then [A-Za-z0-9._-]', () => {
    expect(INPUT_CONTEXT_ID_RE.test('ship')).toBe(true);
    expect(INPUT_CONTEXT_ID_RE.test('Ship.Cockpit_1-a')).toBe(true);
    expect(INPUT_CONTEXT_ID_RE.test('9')).toBe(true);
  });

  it('rejects an empty id, a punctuation-led id and illegal characters', () => {
    expect(INPUT_CONTEXT_ID_RE.test('')).toBe(false);
    expect(INPUT_CONTEXT_ID_RE.test('.ship')).toBe(false);
    expect(INPUT_CONTEXT_ID_RE.test('-ship')).toBe(false);
    expect(INPUT_CONTEXT_ID_RE.test('ship cockpit')).toBe(false);
    expect(INPUT_CONTEXT_ID_RE.test('ship/cockpit')).toBe(false);
  });

  it('rejects an id longer than 64 characters', () => {
    expect(INPUT_CONTEXT_ID_RE.test('a'.repeat(64))).toBe(true);
    expect(INPUT_CONTEXT_ID_RE.test('a'.repeat(65))).toBe(false);
  });

  it('is anchored, so a newline cannot smuggle a legal prefix past it', () => {
    expect(INPUT_CONTEXT_ID_RE.test('ship\nevil id')).toBe(false);
  });
});

describe('dedupeInputContexts', () => {
  it('keeps the FIRST declaration of a duplicate id', () => {
    const out = dedupeInputContexts([
      { id: 'ship', label: 'First' },
      { id: 'ship', label: 'Second', blocksGameplay: true },
    ]);
    expect(out).toEqual([{ id: 'ship', label: 'First', blocksGameplay: false }]);
  });

  it('drops an entry redeclaring the reserved "gameplay" id', () => {
    expect(dedupeInputContexts([{ id: GAMEPLAY_ID, label: 'Hijack', blocksGameplay: true }])).toEqual([]);
  });

  it('drops entries with an invalid id, a non-object entry, and junk', () => {
    expect(dedupeInputContexts([{ id: '.bad' }, null, 'ship', 7, {}, { id: 5 }])).toEqual([]);
  });

  it('falls back to the id for a missing or empty label', () => {
    expect(dedupeInputContexts([{ id: 'ship' }, { id: 'dock', label: '' }])).toEqual([
      { id: 'ship', label: 'ship', blocksGameplay: false },
      { id: 'dock', label: 'dock', blocksGameplay: false },
    ]);
  });

  it('requires blocksGameplay to be strictly true', () => {
    const out = dedupeInputContexts([{ id: 'a', blocksGameplay: 1 }, { id: 'b', blocksGameplay: true }]);
    expect(out.map((c) => c.blocksGameplay)).toEqual([false, true]);
  });

  it('returns [] for a non-array', () => {
    expect(dedupeInputContexts(undefined)).toEqual([]);
    expect(dedupeInputContexts({ id: 'ship' })).toEqual([]);
  });
});

describe('resolveInputContext — the FOUR gameplay fallbacks', () => {
  const declared = schema([{ id: 'ship', label: 'Ship cockpit', blocksGameplay: true }]);
  const fallback = gameplayContext();

  it('1. no inputContext on the setting', () => {
    expect(resolveInputContext(declared, {})).toEqual(fallback);
    expect(resolveInputContext(declared, undefined)).toEqual(fallback);
    const nonString = { inputContext: 7 } as unknown as { inputContext?: string };
    expect(resolveInputContext(declared, nonString)).toEqual(fallback);
  });

  it('2. an explicit "gameplay" resolves WITHOUT consulting the schema', () => {
    // Even a rogue declaration cannot relabel or re-flag the reserved context.
    const rogue = schema([{ id: GAMEPLAY_ID, label: 'Hijacked', blocksGameplay: true }]);
    expect(resolveInputContext(rogue, { inputContext: GAMEPLAY_ID })).toEqual(fallback);
  });

  it('3. an inputContext that fails the id grammar', () => {
    expect(resolveInputContext(declared, { inputContext: '.ship' })).toEqual(fallback);
    expect(resolveInputContext(declared, { inputContext: 'a'.repeat(65) })).toEqual(fallback);
  });

  it('4. a VALID id that the schema does not declare (dangling reference)', () => {
    expect(resolveInputContext(declared, { inputContext: 'dock' })).toEqual(fallback);
    expect(resolveInputContext(schema(undefined), { inputContext: 'ship' })).toEqual(fallback);
    expect(resolveInputContext(undefined, { inputContext: 'ship' })).toEqual(fallback);
    // ...including an id that exists but was DROPPED by the dedupe filter.
    const shadowed = schema([{ id: 'ship' }, { id: 'ship', label: 'Later' }]);
    expect(resolveInputContext(shadowed, { inputContext: 'ship' })?.label).toBe('ship');
  });

  it('resolves a declared context, carrying label and blocksGameplay', () => {
    expect(resolveInputContext(declared, { inputContext: 'ship' })).toEqual({
      id: 'ship',
      label: 'Ship cockpit',
      blocksGameplay: true,
    });
  });

  it('takes the gameplay label from the caller (no localiser in this module)', () => {
    expect(resolveInputContext(declared, {}, 'Spielablauf')).toEqual({
      id: GAMEPLAY_ID,
      label: 'Spielablauf',
      blocksGameplay: false,
    });
  });

  it('every fallback reports id "gameplay", which is what suppresses the badge', () => {
    for (const ref of [undefined, GAMEPLAY_ID, '.bad', 'unknown']) {
      const setting = ref === undefined ? {} : { inputContext: ref };
      expect(resolveInputContext(declared, setting).id).toBe(GAMEPLAY_ID);
    }
  });
});
