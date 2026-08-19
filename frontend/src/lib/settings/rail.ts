
import type { SettingsSchema, SettingValue, ViewsData } from '@sdk';
import { railMatches } from './search';


/** An `osfui/settings` mod record as the renderer actually treats it. */
export interface ModRecord {
  id: string;
  title?: string;
  schema?: SettingsSchema;
  values?: Record<string, SettingValue>;
  targetVersion?: string;
}

/** An `osfui/views` catalog entry, every field optional but `id`. */
export type ViewRecord = Partial<ViewsData['views'][number]> & { id: string };


/** Re-exported so rail consumers need only one import for fixed destination ids. */
export { HEALTH_ID } from './health';

/** The framework's own settings mod id — listed first. */
export const FRAMEWORK_ID = 'osfui';
/** Re-exported so rail consumers need only one import for fixed destination ids. */
export { HOME_ID } from '../ids';

export interface RailEntry {
  id: string;
  /** The settings record, or null for a view-only entry. */
  mod: ModRecord | null;
  views: ViewRecord[];
  title: string;
}

/** Schema title only as a fallback, id as the last resort. */
export function titleOf(mod: ModRecord): string {
  return mod.title || (mod.schema && mod.schema.title) || mod.id;
}

/** Unordered entry set: one per settings mod, plus one per orphaned view group. */
export function railEntries(mods: ModRecord[], views: ViewRecord[]): RailEntry[] {
  const sameMod = (a: string, b: string) => a.toLowerCase() === b.toLowerCase();
  const entries: RailEntry[] = mods.map((m) => ({
    id: m.id,
    mod: m,
    views: views.filter((v) => !!v.mod && sameMod(v.mod, m.id)),
    title: titleOf(m),
  }));

  const orphans = new Map<string, ViewRecord[]>();
  for (const v of views) {
    if (v.mod && mods.some((m) => sameMod(m.id, v.mod!))) continue;
    const key = v.mod || v.id;
    const bucket = orphans.get(key);
    if (bucket) bucket.push(v);
    else orphans.set(key, [v]);
  }
  for (const [key, group] of orphans) {
    const lead = group.find((v) => v.kind === 'menu') || group[0];
    entries.push({
      // "view:" keeps synthetic ids out of the mod-id namespace, as HOME_ID does.
      id: 'view:' + key,
      mod: null,
      views: group,
      title: (lead && lead.title) || key,
    });
  }
  return entries;
}

/** Recomputes the entry set — cheap, and always current. */
export function findEntry(
  mods: ModRecord[],
  views: ViewRecord[],
  id: string | null,
): RailEntry | undefined {
  return railEntries(mods, views).find((e) => e.id === id);
}

export type RailNode =
  | { kind: 'health' }
  | { kind: 'home' }
  | { kind: 'entry'; entry: RailEntry }
  | { kind: 'section' }
  /** `filtered`: a query matched nothing. `none`: nothing is installed. */
  | { kind: 'empty'; reason: 'filtered' | 'none' };

export interface RailModel {
  mods: ModRecord[];
  views: ViewRecord[];
}

export function railNodes(model: RailModel, query: string): RailNode[] {
  const nodes: RailNode[] = [];
  nodes.push({ kind: 'health' });
  if (!query) nodes.push({ kind: 'home' });

  const entries = railEntries(model.mods, model.views);
  for (const e of entries) {
    if (e.id === FRAMEWORK_ID && railMatches(e, query)) nodes.push({ kind: 'entry', entry: e });
  }

  nodes.push({ kind: 'section' });

  const rest = sortedMods(entries, query);
  if (rest.length) {
    for (const e of rest) nodes.push({ kind: 'entry', entry: e });
  } else {
    nodes.push({ kind: 'empty', reason: query ? 'filtered' : 'none' });
  }
  return nodes;
}

/** The non-framework, filter-matching entries in painted order. */
function sortedMods(entries: RailEntry[], query: string): RailEntry[] {
  return entries
    .filter((e) => e.id !== FRAMEWORK_ID && railMatches(e, query))
    .sort((a, b) => a.title.localeCompare(b.title, undefined, { sensitivity: 'base' }));
}
