
import type { SettingsGroup } from '@sdk';
import type { ModRecord, RailEntry } from './rail';
import { titleOf } from './rail';
import { isSetting } from './normalize';

/** A mod's groups, or [] for any missing link in the chain. */
function groupsOf(mod: ModRecord | null | undefined): SettingsGroup[] {
  return (mod && mod.schema && mod.schema.groups) || [];
}

export function railMatches(entry: RailEntry, query: string): boolean {
  if (!query) return true;
  if (entry.title.toLowerCase().includes(query)) return true;
  if (entry.views.some((v) => (v.title || '').toLowerCase().includes(query))) return true;
  for (const g of groupsOf(entry.mod)) {
    for (const s of g.settings || []) {
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
}

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
        out.push({ modId: mod.id, modTitle, groupLabel: g.label || '', key: s.key, label });
      }
    }
  }
  return out;
}
