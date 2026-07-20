// The left-hand mod rail: which entries exist, and in what order the pane
// paints them.
//
// An entry is the union of two independent registries: settings schemas
// (`settings.data`) and catalog views (`views.data`). A view attaches to a
// settings mod when its manifest `mod` matches that mod's id; every other view
// groups into a synthetic "view-only" entry. A mod may ship settings with no
// views, views with no settings, or both.
//
// The model records here are looser than `SettingsDataPayload['mods'][number]`
// / `ViewsDataPayload['views'][number]`: every field may be absent, since the
// renderer also runs against harness mocks and older hosts.

import type { SettingsSchema, SettingValue, ViewsDataPayload, SettingsDataPayload } from '@sdk';
import { railMatches } from './search';

/** A `settings.data` mod record as the renderer actually treats it. */
export interface ModRecord {
  id: string;
  title?: string;
  schema?: SettingsSchema;
  values?: Record<string, SettingValue>;
  targetVersion?: string;
}

/** A `views.data` catalog entry, every field optional but `id`. */
export type ViewRecord = Partial<ViewsDataPayload['views'][number]> & { id: string };

/** A `settings.data` load-failure record. */
export type LoadError = NonNullable<SettingsDataPayload['loadErrors']>[number];

/** The framework's own settings mod id — pinned first. */
export const FRAMEWORK_ID = 'osfui';
/**
 * The Home launcher's rail id. "~" keeps it out of the mod-id namespace (native
 * mod ids can never start with it), so a real mod cannot shadow Home.
 */
export const HOME_ID = '~home';

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
  const entries: RailEntry[] = mods.map((m) => ({
    id: m.id,
    mod: m,
    views: views.filter((v) => v.mod === m.id),
    title: titleOf(m),
  }));

  // Orphans: a view whose `mod` names no loaded settings mod, or which declares
  // no `mod` at all. They group by that string, falling back to the view's own
  // id so a standalone view still gets a rail entry of its own.
  const orphans = new Map<string, ViewRecord[]>();
  for (const v of views) {
    if (v.mod && mods.some((m) => m.id === v.mod)) continue;
    const key = v.mod || v.id;
    const bucket = orphans.get(key);
    if (bucket) bucket.push(v);
    else orphans.set(key, [v]);
  }
  for (const [key, group] of orphans) {
    // A view-only mod has no schema title; borrow its first menu view's title
    // (a menu reads like a product name; a HUD often does not). Falls back to
    // the first view of any kind, then to the grouping key.
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

export { railMatches } from './search';

/**
 * One painted element of the rail, in order. Modelled as data so rendering and
 * `cycleRail` walk the same source.
 */
export type RailNode =
  | { kind: 'loadErrors'; errors: LoadError[] }
  | { kind: 'home' }
  | { kind: 'entry'; entry: RailEntry }
  | { kind: 'section' }
  /** `filtered`: a query matched nothing. `none`: nothing is installed. */
  | { kind: 'empty'; reason: 'filtered' | 'none' };

export interface RailModel {
  mods: ModRecord[];
  views: ViewRecord[];
  loadErrors?: LoadError[];
}

/**
 * The rail in paint order:
 *
 *  1. the load-failure alert, pinned above everything and never filtered — a
 *     user filtering for the mod that failed to load must see why, not
 *     "no mods match" (mcm-design.md §14.2, the SkyUI-MCM lesson);
 *  2. Home, only when no filter is active — while filtering the rail scopes to
 *     matching mods and the launcher steps aside, as the framework does;
 *  3. the framework entry (if it matches), with no header of its own — it
 *     self-labels as "Framework";
 *  4. the "Mods" section header, always emitted, even when the list below it is
 *     empty;
 *  5. every other matching entry, title-sorted case-insensitively.
 *
 * `query` must arrive pre-trimmed and pre-lowercased.
 */
export function railNodes(model: RailModel, query: string): RailNode[] {
  const nodes: RailNode[] = [];
  const errors = model.loadErrors || [];
  if (errors.length) nodes.push({ kind: 'loadErrors', errors });
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
    // sensitivity "base" is case- and accent-insensitive, so "acme" and "Acme"
    // sort adjacently rather than in two ASCII blocks. Undefined locale = the
    // host's, which in game is whatever the WebView reports.
    .sort((a, b) => a.title.localeCompare(b.title, undefined, { sensitivity: 'base' }));
}

/**
 * Move the rail selection by `delta`, wrapping. Returns the id to select, or
 * `null` when nothing should change.
 *
 * Reproduces `railNodes`' order exactly (Home when unfiltered, framework, then
 * title-sorted mods) — the two must agree or LB/RB would skip visible rows.
 *
 * Quirk: when the current selection is not in the list (i < 0) the result is
 * `ids[0]` and `delta` is ignored — LB from an off-list selection moves forward
 * to the first entry, not backward to the last.
 */
export function cycleRail(
  model: RailModel,
  query: string,
  selectedId: string | null,
  delta: number,
): string | null {
  const entries = railEntries(model.mods, model.views);
  const ids: string[] = [];
  if (!query) ids.push(HOME_ID);
  for (const e of entries) {
    if (e.id === FRAMEWORK_ID && railMatches(e, query)) ids.push(e.id);
  }
  for (const e of sortedMods(entries, query)) ids.push(e.id);
  if (!ids.length) return null;

  const i = ids.indexOf(selectedId ?? '');
  const next = ids[i < 0 ? 0 : (i + delta + ids.length) % ids.length];
  if (next === undefined || next === selectedId) return null;
  return next;
}
