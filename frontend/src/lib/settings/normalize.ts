// Renderer-side mirror of SettingsStore::Validate.
//
// The store is authoritative but answers asynchronously, while the pane commits
// optimistically so conditions and modified dots update on the click frame. If
// the local model held the raw pre-clamp value, `sameValue` would see the
// store's echo as an external writer and tear the pane down mid-edit — so
// normalise exactly as native does and the echo becomes a no-op.
//
// Failure mode is refusal, not coercion: a wrong-typed value returns `undefined`
// here just as `Validate` returns `std::nullopt`. The caller must not send it.

import type { Setting, SettingType, SettingValue } from '@sdk';

/** Frozen base type set (sdk/osfui.d.ts `SettingType`). */
export const SETTING_TYPES: readonly SettingType[] = [
  'bool',
  'int',
  'float',
  'enum',
  'flags',
  'string',
  'key',
] as const;

/**
 * Store-wide hard cap on string values (SettingsStore.cpp `kMaxStringLen`).
 * Raising it is a native change — bump this and the harness mock in lockstep.
 */
export const MAX_STRING_LEN = 256;
/** Mirrors SettingsStore.cpp `kMaxKeyNameLen`. */
export const MAX_KEY_NAME_LEN = 16;

/** Colour-widget grammar, shared with the store. */
export const HEX_RE = /^#([0-9a-fA-F]{6}|[0-9a-fA-F]{8})$/;

/**
 * Is this schema item value-bearing (vs. a note/image/action block)?
 * Discriminates purely on `type`, so an item with a type this host predates
 * reads as not-a-setting and is rendered read-only rather than committed.
 */
export function isSetting(item: unknown): item is Setting {
  if (!item || typeof item !== 'object') return false;
  const t = (item as { type?: unknown }).type;
  return typeof t === 'string' && (SETTING_TYPES as readonly string[]).includes(t);
}

/**
 * Normalise `value` for `setting`, or return `undefined` when the store would
 * refuse it outright.
 */
export function normalizeValue(setting: Setting, value: unknown): SettingValue | undefined {
  switch (setting.type) {
    case 'bool':
      // Only a boolean. 1 / "true" are refused, not coerced — nlohmann's
      // is_boolean() is equally strict.
      return typeof value === 'boolean' ? value : undefined;

    case 'int':
    case 'float':
      return normalizeNumber(setting, value);

    case 'enum':
      // Membership in the declared options; no options array => refuse, as
      // SettingsStore does.
      return typeof value === 'string' && Array.isArray(setting.options) && setting.options.includes(value)
        ? value
        : undefined;

    case 'flags':
      return normalizeFlags(setting, value);

    case 'string':
      return normalizeString(setting, value);

    case 'key':
      return normalizeKey(setting, value);

    default:
      // A type this host predates. The store serves it read-only and so do we.
      return undefined;
  }
}

function normalizeNumber(setting: Setting, value: unknown): number | undefined {
  if (typeof value !== 'number' || !Number.isFinite(value)) return undefined;
  // Non-finite is refused rather than clamped: JSON.stringify turns
  // NaN/Infinity into `null`, which native reads as not-a-number and refuses,
  // so it can never come off the wire. Refusing here keeps both ends agreeing
  // about what a local (non-bridge) caller may commit.
  let v = value;
  if (typeof setting.min === 'number') v = Math.max(setting.min, v);
  if (typeof setting.max === 'number') v = Math.min(setting.max, v);
  // Clamp first, round second — an int with min:1 and value 0.4 must land on 1,
  // not 0. SettingsStore uses the same order.
  if (setting.type === 'int') {
    // Native uses std::llround (half away from zero); Math.round is half toward
    // +Infinity. They disagree only on exact .5 at negative values
    // (-0.5 -> -1 native, -0 here). No shipped schema has negative half-steps,
    // so the optimistic value still equals the echo.
    return Math.round(v);
  }
  return v;
}

function normalizeFlags(setting: Setting, value: unknown): string[] | undefined {
  if (!Array.isArray(value) || !Array.isArray(setting.options)) return undefined;
  const wanted = new Set(value.filter((v): v is string => typeof v === 'string'));
  // Iterate the declared options, not the incoming array: filters unknown
  // options, drops non-string junk, dedupes, and canonicalises the order — in
  // exactly the shape SettingsStore produces. The checkbox group matches.
  return setting.options.filter((o) => typeof o === 'string' && wanted.has(o));
}

function normalizeString(setting: Setting, value: unknown): string | undefined {
  if (typeof value !== 'string') return undefined;
  // A colour-widget string must parse as #rrggbb[aa]; the store refuses
  // anything else from any writer, so a bad hex is a refusal, not a silent
  // truncation.
  if (setting.widget === 'color' && !HEX_RE.test(value)) return undefined;
  // Known divergence from native. `||` treats maxLength:0 as unset (cap 256),
  // and a negative maxLength gives a negative cap, where slice(0, -n) chops the
  // last n characters instead of capping. Native applies maxLength only when
  // it is an integer > 0, so both cases make the optimistic value differ from
  // the store's echo:
  //   maxLength:-3  -> here "abcdef" becomes "abc";  native leaves it "abcdef"
  //   maxLength:2.5 -> here slice(0, 2.5) truncates to 2; native ignores it
  // No shipped schema declares either; left unfixed. The fix is to ignore any
  // non-integer or <= 0 maxLength.
  const cap = Math.min(MAX_STRING_LEN, setting.maxLength || MAX_STRING_LEN);
  return value.length > cap ? value.slice(0, cap) : value;
}

function normalizeKey(setting: Setting, value: unknown): string | undefined {
  if (typeof value !== 'string') return undefined;
  if (value === '') {
    // Empty is refused unless the schema opted in, so a blank cannot clobber a
    // working binding by accident. With allowUnbound it is the unbound state
    // the clear button commits.
    return setting.allowUnbound === true ? '' : undefined;
  }
  return value.length > MAX_KEY_NAME_LEN ? value.slice(0, MAX_KEY_NAME_LEN) : value;
}
