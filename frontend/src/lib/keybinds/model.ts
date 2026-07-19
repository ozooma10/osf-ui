// model.ts — flatten a `settings.data` document into keybind rows.
//
// Extracted from src/views/osfui/keybinds/main.legacy.js:75-141.
//
// The keybinds view consumes exactly the same document the settings view does.
// This module turns it into a flat list the board, the detail panel and the
// list all render from: one row per BOUND key, from two sources —
//   * every `type:"key"` setting of every mod (rebindable), and
//   * the top-level `vanillaKeys` table, the game's own bindings (read-only).

import type { SettingsDataPayload, SettingsItem, SettingsSchema } from '@sdk';
import { canonicalName } from './canonical';

/** A mod entry as it arrives on `settings.data`. */
export type ModEntry = SettingsDataPayload['mods'][number];

/**
 * A row of `vanillaKeys`. NOTE the field names: the on-disk curation file
 * (data/OSFUI/vanillakeys.json) uses `label`/`key`, and native renames them to
 * `title`/`name` on the wire. This module sees the WIRE shape.
 */
export type VanillaKey = NonNullable<SettingsDataPayload['vanillaKeys']>[number];

/** Resolved input-context metadata for one row. */
export interface RowContext {
  id: string;
  label: string;
  blocksGameplay: boolean;
}

/** One flattened binding. `mod` is present only on `kind:"mod"` rows. */
export interface BindingRow {
  kind: 'mod' | 'game';
  /** Owning mod id. Absent on game rows (legacy simply never set it). */
  mod?: string;
  /** Mod rows: the setting key. Game rows: the engine controlmap event id. */
  key: string;
  label: string;
  owner: string;
  /** Canonical key name — already alias-folded, see canonicalName(). */
  name: string;
  contextId: string;
  contextLabel: string;
  blocksGameplay: boolean;
}

/**
 * The view's `tr()` shape: a structural address (WITHOUT the
 * "chrome.keybinds." prefix, which the view's own wrapper adds) plus the
 * authored English fallback. Injected so this module stays pure.
 */
export type Translate = (address: string, english: string) => string;

const defaultTranslate: Translate = (_address, english) => english;

/**
 * Input-context ids are constrained so they can be used as stable identifiers
 * (and so a hostile schema cannot smuggle markup into a badge). Legacy line 80.
 */
export const INPUT_CONTEXT_ID_RE = /^[A-Za-z0-9][A-Za-z0-9._-]{0,63}$/;

/**
 * Resolve a key setting's declared `inputContext` against its mod's
 * `schema.inputContexts`. Legacy lines 81-102.
 *
 * Everything degrades to the implicit "gameplay" context: an absent ref, the
 * literal "gameplay" (reserved for the default and never declarable), a ref
 * that fails the id grammar, and a ref that matches no declared context.
 *
 * QUIRK: the fallback's label is the hardcoded English "Gameplay", while the
 * GAME rows below use `tr("gameplay", "Gameplay")`. So in a non-English locale
 * a mod binding in the implicit context shows an untranslated "Gameplay" next
 * to a game binding showing the translated one. Legacy line 82 vs 137.
 * Preserved — it is shipped behaviour and purely cosmetic.
 *
 * QUIRK: the `seen` set and the `id === "gameplay"` / grammar checks are
 * applied while SCANNING, before the `id === ref` comparison. A schema that
 * declares the same id twice therefore has its second entry skipped even
 * though the loop would have returned on the first anyway — the dedupe is
 * dead weight here, but it is copied verbatim rather than "cleaned up"
 * because the same scan shape is what the settings view uses.
 */
export function inputContextFor(
  schema: SettingsSchema | undefined,
  inputContext: string | undefined,
): RowContext {
  const fallback: RowContext = { id: 'gameplay', label: 'Gameplay', blocksGameplay: false };
  const ref = typeof inputContext === 'string' ? inputContext : '';
  if (!ref || ref === 'gameplay' || !INPUT_CONTEXT_ID_RE.test(ref)) return fallback;

  const contexts = Array.isArray(schema?.inputContexts) ? schema.inputContexts : [];
  const seen = new Set<string>();
  for (const context of contexts) {
    if (!context || typeof context !== 'object') continue;
    const id = typeof context.id === 'string' ? context.id : '';
    if (id === 'gameplay' || !INPUT_CONTEXT_ID_RE.test(id) || seen.has(id)) continue;
    seen.add(id);
    if (id === ref) {
      return {
        id,
        // An empty-string label falls back to the id, not to "" — `&& context.label`.
        label: typeof context.label === 'string' && context.label ? context.label : id,
        // Strict `=== true`: a truthy non-boolean does NOT grant the assertion.
        blocksGameplay: context.blocksGameplay === true,
      };
    }
  }
  return fallback;
}

/**
 * "Starfield (Quicksave)" -> "Quicksave", for display inside a GAME-tagged row
 * where repeating "Starfield" on every line would be noise. Legacy lines 75-78.
 *
 * The regex requires at least one char before " (" and at least one inside the
 * parens, and `[^(]+` forbids a nested open paren in the prefix. Anything that
 * does not match is passed through WHOLE — that is the intended behaviour for
 * a title native did not format that way, not a fallback for a broken parse.
 */
export function vanillaLabel(title: unknown): string {
  const s = String(title || '');
  const m = /^[^(]+ \((.+)\)$/.exec(s);
  return m?.[1] ?? s;
}

/** Narrow a group item to a key-typed Setting with a usable `key`. */
function isKeySetting(item: SettingsItem | null | undefined): item is Extract<
  SettingsItem,
  { type: string }
> & { key: string; type: 'key'; label?: string; inputContext?: string } {
  if (!item || typeof item !== 'object') return false;
  const s = item as { type?: unknown; key?: unknown };
  return s.type === 'key' && typeof s.key === 'string';
}

/**
 * Build the flat binding list.
 *
 * Mod rows come first, in schema order (mod, then group, then setting); game
 * rows are appended after. That order is load-bearing for the detail panel,
 * which renders holders in list order, and for the stable tie-break in
 * compareBindings().
 *
 * IMPORTANT: a row exists only for a key setting whose value is a NON-EMPTY
 * string (legacy line 113). An unbound key — the `allowUnbound` state, stored
 * as "" — produces NO row at all. That is why an unbound setting cannot
 * conflict with anything and never appears on the board or in the list, and it
 * is also why the list count can be lower than the number of key settings.
 */
export function buildModel(
  mods: readonly ModEntry[] | null | undefined,
  vanillaKeys: readonly VanillaKey[] | null | undefined,
  translate: Translate = defaultTranslate,
): BindingRow[] {
  const rows: BindingRow[] = [];

  // `||` rather than `??` throughout, matching legacy 109/110/112 exactly: a
  // schema whose `groups` is any falsy non-nullish value (0, "", false — a
  // hand-edited or hostile manifest) degrades to [] here, where `??` would let
  // it through to the for-of and throw. Same for `settings` and `values`.
  //
  // DIVERGENCE (small, deliberate): the `if (!mod)` / `if (!v)` guards below
  // have no legacy counterpart — legacy dereferenced `mod.schema` / `v.event`
  // directly, so a null entry threw a TypeError out of buildModel and took the
  // whole render with it. Native never sends one, and a crashed render is not
  // behaviour worth preserving bug-for-bug, so a null entry is skipped instead.
  for (const mod of mods || []) {
    if (!mod) continue;
    for (const g of mod.schema?.groups || []) {
      for (const s of g?.settings || []) {
        if (!isKeySetting(s)) continue;
        const value = (mod.values || {})[s.key];
        if (typeof value !== 'string' || !value) continue; // unbound => no row
        const context = inputContextFor(mod.schema, s.inputContext);
        rows.push({
          kind: 'mod',
          mod: mod.id,
          key: s.key,
          // An absent OR empty label degrades to the raw setting key, so a row
          // is never blank in the UI.
          label: s.label || s.key,
          owner: mod.title || mod.id,
          name: canonicalName(value),
          contextId: context.id,
          contextLabel: context.label,
          blocksGameplay: context.blocksGameplay,
        });
      }
    }
  }

  for (const v of vanillaKeys || []) {
    if (!v) continue;
    rows.push({
      kind: 'game',
      // Game rows carry the engine controlmap event id in `key` and no `mod`.
      key: v.event,
      label: vanillaLabel(v.title),
      owner: translate('gameOwner', 'Starfield'),
      name: canonicalName(v.name),
      // Vanilla bindings are ALWAYS plain gameplay: the game has no input
      // contexts in this model, so they can never be the blocksGameplay side
      // of a shared pair. See pairIsShared() in conflicts.ts. Legacy 136-138.
      contextId: 'gameplay',
      contextLabel: translate('gameplay', 'Gameplay'),
      blocksGameplay: false,
    });
  }

  return rows;
}
