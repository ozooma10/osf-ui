// model.ts — flatten a `settings.data` document into keybind rows.
//
// The keybinds view consumes the same document the settings view does. One row
// per bound key, from two sources: every `type:"key"` setting of every mod
// (rebindable), and the top-level `vanillaKeys` table (the game's own bindings,
// read-only).

import type { SettingsDataPayload, SettingsItem, SettingsSchema } from '@sdk';
import { canonicalName } from './canonical';

export type ModEntry = SettingsDataPayload['mods'][number];

/**
 * A row of `vanillaKeys`. The on-disk curation file (data/OSFUI/vanillakeys.json)
 * uses `label`/`key`; native renames them to `title`/`name` on the wire. This is
 * the wire shape.
 */
export type VanillaKey = NonNullable<SettingsDataPayload['vanillaKeys']>[number];

export interface RowContext {
  id: string;
  label: string;
  blocksGameplay: boolean;
}

/** One flattened binding. `mod` is present only on `kind:"mod"` rows. */
export interface BindingRow {
  kind: 'mod' | 'game';
  /** Owning mod id. Absent on game rows. */
  mod?: string;
  /** Mod rows: the setting key. Game rows: the engine controlmap event id. */
  key: string;
  label: string;
  owner: string;
  /** Canonical key name, already alias-folded by canonicalName(). */
  name: string;
  contextId: string;
  contextLabel: string;
  blocksGameplay: boolean;
}

/**
 * The view's `tr()` shape: a structural address (without the "chrome.keybinds."
 * prefix, which the view's own wrapper adds) plus the authored English
 * fallback. Injected to keep this module pure.
 */
export type Translate = (address: string, english: string) => string;

const defaultTranslate: Translate = (_address, english) => english;

/**
 * Input-context ids are constrained so they can be used as stable identifiers,
 * and so a hostile schema cannot smuggle markup into a badge.
 */
export const INPUT_CONTEXT_ID_RE = /^[A-Za-z0-9][A-Za-z0-9._-]{0,63}$/;

/**
 * Resolve a key setting's declared `inputContext` against its mod's
 * `schema.inputContexts`. Everything degrades to the implicit "gameplay"
 * context: an absent ref, the literal "gameplay" (reserved for the default and
 * never declarable), a ref that fails the id grammar, and a ref that matches no
 * declared context.
 *
 * Quirk: the fallback label is hardcoded English "Gameplay" while the game rows
 * below use `tr("gameplay", "Gameplay")`, so in a non-English locale a mod
 * binding in the implicit context shows an untranslated "Gameplay" next to a
 * translated one. Shipped behaviour, cosmetic only.
 *
 * Quirk: the `seen` set and the `id === "gameplay"` / grammar checks run while
 * scanning, before the `id === ref` comparison, so a schema that declares the
 * same id twice skips its second entry. The dedupe is dead weight here; it
 * matches the scan shape the settings view uses.
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
        // Strict `=== true`: a truthy non-boolean does not grant the assertion.
        blocksGameplay: context.blocksGameplay === true,
      };
    }
  }
  return fallback;
}

/**
 * "Starfield (Quicksave)" -> "Quicksave", for display inside a game-tagged row
 * where repeating "Starfield" on every line would be noise.
 *
 * The regex requires at least one char before " (" and at least one inside the
 * parens, and `[^(]+` forbids a nested open paren in the prefix. Anything that
 * does not match is passed through whole — intended for a title native did not
 * format that way, not a fallback for a broken parse.
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
 * A row exists only for a key setting whose value is a non-empty string. An
 * unbound key — the `allowUnbound` state, stored as "" — produces no row at
 * all, so it cannot conflict with anything and never appears on the board or in
 * the list; the list count can therefore be lower than the number of key
 * settings.
 */
export function buildModel(
  mods: readonly ModEntry[] | null | undefined,
  vanillaKeys: readonly VanillaKey[] | null | undefined,
  translate: Translate = defaultTranslate,
): BindingRow[] {
  const rows: BindingRow[] = [];

  // `||` rather than `??` throughout: a schema whose `groups` is any falsy
  // non-nullish value (0, "", false — a hand-edited or hostile manifest)
  // degrades to [] here, where `??` would let it through to the for-of and
  // throw. Same for `settings` and `values`.
  //
  // The `if (!mod)` / `if (!v)` guards skip a null entry rather than letting a
  // TypeError escape buildModel and take the whole render with it. Native never
  // sends one.
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
          // An absent or empty label degrades to the raw setting key, so a row
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
      // Vanilla bindings are always plain gameplay: the game has no input
      // contexts in this model, so they can never be the blocksGameplay side
      // of a shared pair. See pairIsShared() in conflicts.ts.
      contextId: 'gameplay',
      contextLabel: translate('gameplay', 'Gameplay'),
      blocksGameplay: false,
    });
  }

  return rows;
}
