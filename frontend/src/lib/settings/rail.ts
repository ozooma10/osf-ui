// rail.ts — the left-hand mod rail: which entries exist, and in what order the
// pane paints them (main.legacy.js:747-927 and 1875-1889).
//
// An entry is the UNION of two independent registries: settings schemas
// (`settings.data`) and catalog views (`views.data`). A view attaches to a
// settings mod when its manifest `mod` matches that mod's id; every other view
// groups into a synthetic "view-only" entry. Neither registry is authoritative
// on its own — a mod may ship settings with no views, views with no settings,
// or both.
//
// This module also hosts the loose model records the other settings modules
// share. They are deliberately LOOSER than `SettingsDataPayload['mods'][number]`
// / `ViewsDataPayload['views'][number]`: the legacy renderer defends against
// every field being absent (it runs against harness mocks and against older
// hosts), and the ports must be able to reproduce those branches.

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

/** The framework's own settings mod id — pinned first (main.legacy.js:32). */
export const FRAMEWORK_ID = 'osfui';
/**
 * The Home launcher's rail id. "~" keeps it out of the mod-id namespace (native
 * mod ids can never start with it), so a real mod cannot shadow Home.
 * main.legacy.js:35.
 */
export const HOME_ID = '~home';

export interface RailEntry {
  id: string;
  /** The settings record, or null for a view-only entry. */
  mod: ModRecord | null;
  /** The catalog views attached to this entry. */
  views: ViewRecord[];
  title: string;
}

/** main.legacy.js:186 — schema title only as a fallback, id as the last resort. */
export function titleOf(mod: ModRecord): string {
  return mod.title || (mod.schema && mod.schema.title) || mod.id;
}

/**
 * The unordered entry set: one per settings mod, plus one per orphaned view
 * group.
 */
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
    // A view-only mod has no schema title; borrow its first PANEL's title (a
    // menu reads like a product name; a HUD often does not). Falls back to the
    // first view of any kind, then to the grouping key.
    const lead = group.find((v) => v.kind === 'menu') || group[0];
    entries.push({
      // The "view:" prefix keeps synthetic ids out of the mod-id namespace, the
      // same trick HOME_ID uses.
      id: 'view:' + key,
      mod: null,
      views: group,
      title: (lead && lead.title) || key,
    });
  }
  return entries;
}

/** main.legacy.js:771. Recomputes the entry set — cheap, and always current. */
export function findEntry(
  mods: ModRecord[],
  views: ViewRecord[],
  id: string | null,
): RailEntry | undefined {
  return railEntries(mods, views).find((e) => e.id === id);
}

export { railMatches } from './search';

/**
 * One painted element of the rail, in order. Modelled as data so the port can
 * render it and `cycleRail` can walk it from the same source.
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
 * The rail exactly as `renderRail` paints it (main.legacy.js:896-927):
 *
 *  1. the load-failure alert, PINNED ABOVE EVERYTHING and never filtered — a
 *     user filtering for the mod that failed to load must see WHY, not
 *     "no mods match" (mcm-design.md §14.2, the SkyUI-MCM lesson);
 *  2. Home, but ONLY when no filter is active — while filtering the rail scopes
 *     to matching mods and the launcher steps aside like the framework does;
 *  3. the framework entry (if it matches), with no header of its own — it
 *     self-labels as "Framework";
 *  4. the "Mods" section header, ALWAYS emitted, even when the list below it is
 *     empty;
 *  5. every other matching entry, title-sorted case-insensitively.
 *
 * `query` is expected pre-trimmed and pre-lowercased, as the legacy caller
 * passes it (main.legacy.js:897).
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
    // `localeCompare(undefined, { sensitivity: "base" })`: case- AND
    // accent-insensitive, so "acme" and "Acme" sort adjacently rather than in
    // two ASCII blocks. `undefined` locale = the host's, which in game is
    // whatever the WebView reports.
    .sort((a, b) => a.title.localeCompare(b.title, undefined, { sensitivity: 'base' }));
}

/**
 * Move the rail selection by `delta`, wrapping. Returns the id to select, or
 * `null` when nothing should change. main.legacy.js:1875-1889.
 *
 * Reproduces `railNodes`' order exactly (Home when unfiltered, framework, then
 * title-sorted mods) — the two MUST agree or LB/RB would skip visible rows.
 *
 * QUIRK: when the current selection is not in the list (i < 0), the result is
 * `ids[0]` with `delta` IGNORED — pressing LB from an off-list selection moves
 * FORWARD to the first entry, not backward to the last.
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
