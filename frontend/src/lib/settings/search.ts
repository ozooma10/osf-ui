// search.ts — the filter box. Two consumers, two behaviours:
//
//  * `railMatches` scopes the RAIL to entries that match (main.legacy.js:773).
//  * `searchResults` replaces the DETAIL pane with a flat, cross-mod result
//    list (main.legacy.js:1384-1426).
//
// Both take a query that the caller has already trimmed and lowercased, exactly
// as the legacy view does once at main.legacy.js:897/1090. They do NOT lowercase
// it again — pass a raw mixed-case query and nothing will match.

import type { SettingsGroup } from '@sdk';
import type { ModRecord, RailEntry } from './rail';
import { titleOf } from './rail';
import { isSetting } from './normalize';

/** The groups of a mod, defended the way the legacy renderer defends them. */
function groupsOf(mod: ModRecord | null | undefined): SettingsGroup[] {
  return (mod && mod.schema && mod.schema.groups) || [];
}

/**
 * Does this rail entry survive the filter? An empty query matches everything.
 *
 * Matching is deliberately WIDE: the entry title, any attached view's title,
 * and any of the mod's setting labels. Filtering for a setting name therefore
 * leaves the owning mod visible in the rail, so the result list in the detail
 * pane and the rail agree about which mods are involved.
 */
export function railMatches(entry: RailEntry, query: string): boolean {
  if (!query) return true;
  if (entry.title.toLowerCase().includes(query)) return true;
  if (entry.views.some((v) => (v.title || '').toLowerCase().includes(query))) return true;
  for (const g of groupsOf(entry.mod)) {
    for (const s of g.settings || []) {
      // NOTE: unlike the cross-mod scan below this does NOT require `isSetting`
      // or a non-empty key — a note/action/image item whose `label` matches
      // keeps its mod in the rail. Faithful to main.legacy.js:777-781, where
      // the loop reads `s.label || s.key || ""` off any item.
      //
      // ONE hardening beyond legacy: the `typeof === 'string'` guards. Legacy
      // called `.toLowerCase()` on whatever `label || key` produced, so a schema
      // with a NUMERIC label threw a TypeError out of `renderRail` and left the
      // rail empty. Coercing to "" instead only ever converts a crash into a
      // non-match, and the schema validator rejects such labels upstream anyway.
      const item = s as { label?: unknown; key?: unknown };
      const text = (typeof item.label === 'string' && item.label) ||
        (typeof item.key === 'string' && item.key) || '';
      if (text.toLowerCase().includes(query)) return true;
    }
  }
  return false;
}

/** One row of the cross-mod result list. */
export interface SearchResult {
  modId: string;
  modTitle: string;
  /** The owning group's label, or "" when the group is unlabelled. */
  groupLabel: string;
  /** The setting key — always a non-empty string (that is the scan's filter). */
  key: string;
  /** `label || key`, the text the row displays. */
  label: string;
  /** "mod title › group label", or just the mod title for an unlabelled group. */
  breadcrumb: string;
}

/**
 * Flat, cross-mod settings search (main.legacy.js:1395-1422).
 *
 * The scan keeps ONLY real settings with a non-empty string key — notes,
 * images, actions and keyless settings are skipped, because clicking a result
 * jumps to `.row[data-key=...]` and a row without a key has no anchor to jump
 * to.
 *
 * A row is a hit when the query matches the setting's own text OR the owning
 * mod's TITLE — so searching a mod name lists every one of its settings, which
 * is what makes the filter usable as "show me everything in this mod".
 */
export function searchResults(mods: ModRecord[], query: string): SearchResult[] {
  const out: SearchResult[] = [];
  for (const mod of mods) {
    const modTitle = titleOf(mod);
    const modMatches = modTitle.toLowerCase().includes(query);
    for (const g of groupsOf(mod)) {
      for (const s of g.settings || []) {
        if (!isSetting(s) || typeof s.key !== 'string' || !s.key) continue;
        const label = s.label || s.key || '';
        if (!label.toLowerCase().includes(query) && !modMatches) continue;
        const groupLabel = g.label || '';
        out.push({
          modId: mod.id,
          modTitle,
          groupLabel,
          key: s.key,
          label,
          breadcrumb: groupLabel ? `${modTitle} › ${groupLabel}` : modTitle,
        });
      }
    }
  }
  return out;
}
