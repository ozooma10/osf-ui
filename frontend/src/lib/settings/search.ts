// Filter box. `railMatches` scopes the rail to matching entries;
// `searchResults` replaces the detail pane with a flat, cross-mod result list.
//
// Both expect a query the caller has already trimmed and lowercased. Neither
// lowercases again — a raw mixed-case query matches nothing.

import type { SettingsGroup } from '@sdk';
import type { ModRecord, RailEntry } from './rail';
import { titleOf } from './rail';
import { isSetting } from './normalize';

/** A mod's groups, or [] for any missing link in the chain. */
function groupsOf(mod: ModRecord | null | undefined): SettingsGroup[] {
  return (mod && mod.schema && mod.schema.groups) || [];
}

/**
 * Does this rail entry survive the filter? An empty query matches everything.
 *
 * Matching is wide — entry title, any attached view's title, any of the mod's
 * setting labels — so filtering for a setting name keeps the owning mod in the
 * rail and the rail agrees with the detail pane's result list.
 */
export function railMatches(entry: RailEntry, query: string): boolean {
  if (!query) return true;
  if (entry.title.toLowerCase().includes(query)) return true;
  if (entry.views.some((v) => (v.title || '').toLowerCase().includes(query))) return true;
  for (const g of groupsOf(entry.mod)) {
    for (const s of g.settings || []) {
      // Unlike the cross-mod scan below, this requires neither `isSetting` nor
      // a non-empty key: a note/action/image item whose `label` matches keeps
      // its mod in the rail.
      //
      // The `typeof === 'string'` guards matter — calling `.toLowerCase()` on a
      // numeric label throws out of the rail render and leaves the rail empty.
      // Coercing to "" turns that crash into a non-match. (The schema validator
      // rejects such labels upstream anyway.)
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
  /** Setting key; always a non-empty string (that is the scan's filter). */
  key: string;
  /** `label || key`, the text the row displays. */
  label: string;
  /** "mod title › group label", or just the mod title for an unlabelled group. */
  breadcrumb: string;
}

/**
 * Flat, cross-mod settings search.
 *
 * Keeps only real settings with a non-empty string key; notes, images, actions
 * and keyless settings are skipped because clicking a result jumps to
 * `.row[data-key=...]` and a keyless row has no anchor.
 *
 * A row hits when the query matches the setting's own text or the owning mod's
 * title, so searching a mod name lists every one of its settings.
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
