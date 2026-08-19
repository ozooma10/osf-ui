
import type {
  GameplayMode,
  GameInputContextClassification,
  KeybindingsData,
  SettingsData,
  SettingsItem,
} from '@sdk';
import { canonicalName } from './canonical';
import type { KeyLabeler } from './labels';
import { resolveHotkeyContext } from '../settings/hotkeyContext';

export type ModEntry = SettingsData['mods'][number];

/** Fields shared by authored mod hotkeys and live Starfield bindings. */
interface BindingRowBase {
  /** Mod rows: the setting key. Game rows: the engine controlmap event id. */
  key: string;
  label: string;
  owner: string;
  /** Canonical key name, already alias-folded by canonicalName(). */
  name: string;
  keyLabel: string;
  /** null is legacy/unscoped and overlaps every semantic mode. */
  gameplayModes?: GameplayMode[] | null;
  chord?: string[];
  unbound?: boolean;
  /** False when the compatibility setting `vanillaKeyConflicts` hides game-binding warnings. */
  gameBindingWarnings?: boolean;
  /** Stable rendered identity; game input actions may have main + alternate rows. */
  rowId?: string;
}

/** An authored `type:"key"` setting and its mod-local hotkey context. */
export interface ModBindingRow extends BindingRowBase {
  kind: 'mod';
  /** Owning mod id. */
  mod: string;
  hotkeyContextId: string;
  hotkeyContextLabel: string;
  blocksGameplay: boolean;
}

/** A live Starfield ControlMap binding and its engine input context. */
export interface GameBindingRow extends BindingRowBase {
  kind: 'game';
  /** Stable engine input-context name, or `unknown` when the game did not supply one. */
  engineInputContextName: string;
  /** Display label, localized only for the missing-context fallback. */
  engineInputContextLabel: string;
  engineInputContextId?: number;
  classification?: GameInputContextClassification;
  category?: string;
  slot?: 'main' | 'alternate';
}

/** One flattened binding, discriminated by its domain owner. */
export type BindingRow = ModBindingRow | GameBindingRow;

export type Translate = (address: string, english: string) => string;

const defaultTranslate: Translate = (_address, english) => english;

/** Narrow a group item to a key-typed Setting with a usable `key`. */
function isKeySetting(item: SettingsItem | null | undefined): item is Extract<
  SettingsItem,
  { type: string }
> & { key: string; type: 'key'; label?: string; inputContext?: string } {
  if (!item || typeof item !== 'object') return false;
  const s = item as { type?: unknown; key?: unknown };
  return s.type === 'key' && typeof s.key === 'string';
}

export function buildModel(
  mods: readonly ModEntry[] | null | undefined,
  gameActions: readonly KeybindingsData['actions'][number][] | null | undefined,
  translate: Translate = defaultTranslate,
  labeler: KeyLabeler = () => undefined,
): BindingRow[] {
  const rows: BindingRow[] = [];
  const osfui = (mods || []).find((m) => m?.id === 'osfui');
  const gameBindingWarnings = osfui?.values?.vanillaKeyConflicts !== false;

  for (const mod of mods || []) {
    if (!mod) continue;
    for (const g of mod.schema?.groups || []) {
      for (const s of g?.settings || []) {
        if (!isKeySetting(s)) continue;
        const value = (mod.values || {})[s.key];
        if (typeof value !== 'string' || !value) continue; // unbound => no row
        const context = resolveHotkeyContext(mod.schema, s, translate('gameplay', 'Gameplay'));
        const name = canonicalName(value);
        rows.push({
          kind: 'mod',
          mod: mod.id,
          key: s.key,
          label: s.label || s.key,
          owner: mod.title || mod.id,
          name,
          keyLabel: labeler(name) ?? name,
          hotkeyContextId: context.id,
          hotkeyContextLabel: context.label,
          blocksGameplay: context.blocksGameplay,
          gameplayModes: context.gameplayModes ?? null,
          chord: [name],
          unbound: false,
          gameBindingWarnings,
          rowId: `mod:${mod.id}:${s.key}`,
        });
      }
    }
  }

  for (const v of gameActions || []) {
    if (!v) continue;
    if (Array.isArray(v.bindings)) {
      for (let i = 0; i < v.bindings.length; ++i) {
        const binding = v.bindings[i];
        if (!binding) continue;
        const chord = Array.isArray(binding.chord)
          ? binding.chord.filter((x): x is string => typeof x === 'string' && !!x).map(canonicalName)
          : [];
        const name = !binding.unbound && chord.length === 1 ? chord[0] || '' : '';
        const modes = [...(v.modes?.definite || []), ...(v.modes?.possible || [])];
        rows.push({
          kind: 'game',
          key: v.event,
          label: v.label || v.event,
          owner: translate('gameOwner', 'Starfield'),
          name,
          keyLabel: binding.unbound
            ? translate('unbound', 'Unbound')
            : chord.map((part) => labeler(part) ?? part).join(' + '),
          engineInputContextName: v.context?.name || 'unknown',
          engineInputContextLabel: v.context?.name || translate('otherContext', 'Other'),
          engineInputContextId: v.context?.id,
          category: v.category,
          classification: v.classification,
          gameplayModes: [...new Set(modes)],
          slot: binding.slot,
          chord,
          unbound: binding.unbound || chord.length === 0,
          gameBindingWarnings,
          rowId: `game:${v.context?.id ?? 'x'}:${v.event}:${binding.slot}:${i}`,
        });
      }
    }
  }

  return rows;
}
