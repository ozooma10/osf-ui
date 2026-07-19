// normalize.ts — the renderer-side mirror of SettingsStore::Validate
// (src/runtime/SettingsStore.cpp:1011-1114).
//
// WHY a second copy exists at all: the store is authoritative, but it answers
// asynchronously. The pane commits optimistically so conditions and modified
// dots update on the same frame as the click (main.legacy.js:162-177). If the
// local model held the raw pre-clamp value it would disagree with the store for
// a round trip, and `sameValue` would then report a spurious "external writer"
// and tear the pane down mid-edit (main.legacy.js:1793-1801). So we normalise
// the same way native does, and the echo is a no-op.
//
// REFUSAL, not coercion, is the failure mode: a value of the wrong JS type
// returns `undefined` here exactly as `Validate` returns `std::nullopt`. The
// caller must not send it.

import type { Setting, SettingType, SettingValue } from '@sdk';

/** The frozen base type set (sdk/osfui.d.ts `SettingType`). */
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
 * Store-wide hard cap on string values (SettingsStore.cpp:16 `kMaxStringLen`).
 * Raising it is a NATIVE change — bump this and the harness mock in lockstep.
 */
export const MAX_STRING_LEN = 256;
/** SettingsStore.cpp:1105 `kMaxKeyNameLen`. */
export const MAX_KEY_NAME_LEN = 16;

/** main.legacy.js:40 — the colour-widget grammar, shared with the store. */
export const HEX_RE = /^#([0-9a-fA-F]{6}|[0-9a-fA-F]{8})$/;

/**
 * Is this schema item a value-bearing setting (as opposed to a note/image/
 * action block)? main.legacy.js:292-295. Discriminates purely on `type`, so an
 * item with a type this host predates reads as NOT a setting and is rendered
 * read-only rather than committed.
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
      // Strict: only a real boolean. 1 / "true" are REFUSED, not coerced —
      // nlohmann's is_boolean() is equally strict.
      return typeof value === 'boolean' ? value : undefined;

    case 'int':
    case 'float':
      return normalizeNumber(setting, value);

    case 'enum':
      // Membership in the declared options. No options array => refuse
      // (SettingsStore.cpp:1035-1042 falls through to nullopt).
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
      // A type this host predates. The store serves it read-only ("read-only",
      // SettingsStore.cpp:1145-1149) and so do we: never commit it.
      return undefined;
  }
}

function normalizeNumber(setting: Setting, value: unknown): number | undefined {
  if (typeof value !== 'number' || !Number.isFinite(value)) return undefined;
  // Non-finite is refused rather than clamped because it can never have come
  // off the wire: JSON.stringify turns NaN/Infinity into `null`, which native
  // reads as not-a-number and refuses. Refusing here keeps the two ends
  // agreeing about what a local (non-bridge) caller may commit.
  let v = value;
  if (typeof setting.min === 'number') v = Math.max(setting.min, v);
  if (typeof setting.max === 'number') v = Math.min(setting.max, v);
  // CLAMP FIRST, ROUND SECOND — an int with min:1 and value 0.4 must land on 1,
  // not on 0. SettingsStore.cpp:1022-1030 has the same order.
  if (setting.type === 'int') {
    // Native uses std::llround (half away from zero); Math.round is half toward
    // +Infinity. They disagree only on exact .5 at negative values
    // (-0.5 -> -1 native, -0 here). Left as-is: the legacy renderer used
    // Math.round everywhere (main.legacy.js:257, 376) and matching IT is what
    // keeps the optimistic value equal to the echo in every shipped schema
    // (none of which have negative half-steps).
    return Math.round(v);
  }
  return v;
}

function normalizeFlags(setting: Setting, value: unknown): string[] | undefined {
  if (!Array.isArray(value) || !Array.isArray(setting.options)) return undefined;
  const wanted = new Set(value.filter((v): v is string => typeof v === 'string'));
  // Iterate the DECLARED options, not the incoming array: that filters unknown
  // options, drops non-string junk, dedupes (a Set membership test can only
  // emit each option once), and canonicalises the ORDER — all in one pass, and
  // in exactly the shape SettingsStore.cpp:1050-1066 produces. The rendered
  // checkbox group does the same thing (main.legacy.js:444).
  return setting.options.filter((o) => typeof o === 'string' && wanted.has(o));
}

function normalizeString(setting: Setting, value: unknown): string | undefined {
  if (typeof value !== 'string') return undefined;
  // A colour-widget string must parse as #rrggbb[aa]; the store refuses
  // anything else from ANY writer (SettingsStore.cpp:1073), so a bad hex is a
  // refusal, not a silent truncation.
  if (setting.widget === 'color' && !HEX_RE.test(value)) return undefined;
  // QUIRK, deliberately preserved from main.legacy.js:458:
  //   Math.min(256, setting.maxLength || 256)
  // `||` means maxLength:0 is treated as "unset" (cap 256), while a NEGATIVE
  // maxLength yields a negative cap and `slice(0, -n)` then chops the LAST n
  // characters instead of capping. Native disagrees (SettingsStore.cpp:1080-1084
  // applies maxLength only when `is_number_integer()` AND `v > 0`), so BOTH a
  // negative and a FRACTIONAL maxLength make the optimistic value differ from
  // the store's echo:
  //   maxLength:-3  -> here "abcdef" becomes "abc";  native leaves it "abcdef"
  //   maxLength:2.5 -> here slice(0, 2.5) truncates to 2; native ignores it
  // No shipped schema declares either. Fixing it means aligning with native
  // (ignore any non-integer or <= 0 maxLength) — deliberately NOT done, because
  // this is a shipped view and the legacy renderer is the source of truth.
  const cap = Math.min(MAX_STRING_LEN, setting.maxLength || MAX_STRING_LEN);
  return value.length > cap ? value.slice(0, cap) : value;
}

function normalizeKey(setting: Setting, value: unknown): string | undefined {
  if (typeof value !== 'string') return undefined;
  if (value === '') {
    // Empty is REFUSED unless the schema opted in — a blank must never clobber
    // a working binding by accident (SettingsStore.cpp:1094-1103). With
    // allowUnbound it is the deliberate unbound state the ✕ button commits
    // (main.legacy.js:508-520).
    return setting.allowUnbound === true ? '' : undefined;
  }
  return value.length > MAX_KEY_NAME_LEN ? value.slice(0, MAX_KEY_NAME_LEN) : value;
}
