
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

export const MAX_STRING_LEN = 256;
/** Mirrors SettingsStore.cpp `kMaxKeyNameLen`. */
export const MAX_KEY_NAME_LEN = 16;

/** Colour-widget grammar, shared with the store. */
export const HEX_RE = /^#([0-9a-fA-F]{6}|[0-9a-fA-F]{8})$/;

export function isSetting(item: unknown): item is Setting {
  if (!item || typeof item !== 'object') return false;
  const t = (item as { type?: unknown }).type;
  return typeof t === 'string' && (SETTING_TYPES as readonly string[]).includes(t);
}

export function normalizeValue(setting: Setting, value: unknown): SettingValue | undefined {
  switch (setting.type) {
    case 'bool':
      return typeof value === 'boolean' ? value : undefined;

    case 'int':
    case 'float':
      return normalizeNumber(setting, value);

    case 'enum':
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
      // A type this OSF UI runtime predates. The store serves it read-only and so do we.
      return undefined;
  }
}

function normalizeNumber(setting: Setting, value: unknown): number | undefined {
  if (typeof value !== 'number' || !Number.isFinite(value)) return undefined;
  let v = value;
  if (typeof setting.min === 'number') v = Math.max(setting.min, v);
  if (typeof setting.max === 'number') v = Math.min(setting.max, v);
  if (setting.type === 'int') {
    return Math.round(v);
  }
  return v;
}

function normalizeFlags(setting: Setting, value: unknown): string[] | undefined {
  if (!Array.isArray(value) || !Array.isArray(setting.options)) return undefined;
  const wanted = new Set(value.filter((v): v is string => typeof v === 'string'));
  return setting.options.filter((o) => typeof o === 'string' && wanted.has(o));
}

function normalizeString(setting: Setting, value: unknown): string | undefined {
  if (typeof value !== 'string') return undefined;
  if (setting.widget === 'color' && !HEX_RE.test(value)) return undefined;
  const cap = Math.min(MAX_STRING_LEN, setting.maxLength || MAX_STRING_LEN);
  return value.length > cap ? value.slice(0, cap) : value;
}

function normalizeKey(setting: Setting, value: unknown): string | undefined {
  if (typeof value !== 'string') return undefined;
  if (value === '') {
    return setting.allowUnbound === true ? '' : undefined;
  }
  return value.length > MAX_KEY_NAME_LEN ? value.slice(0, MAX_KEY_NAME_LEN) : value;
}
