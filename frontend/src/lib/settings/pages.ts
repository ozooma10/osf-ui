// Page segmentation for a mod's settings schema (`schema.pages` + each
// group's `page` field).
//
// Pages are pure display structure, annotated onto the existing flat group
// list rather than nesting it: an OSF UI runtime that predates them ignores the unknown
// fields and renders the plain group column, so a paged schema degrades to
// exactly the pre-pages UI. That is why `page` lives ON the group instead of
// groups living inside `pages[]`.
//
// Assignment rules, in schema order:
//   - a group whose `page` names a declared page id lands on that page
//   - a group with no `page`, or an unknown/invalid one, lands on the
//     implicit General page, which paints FIRST when it has content
//   - a declared page no group references is dropped (no empty tabs)
//   - fewer than two non-empty pages is not a segmentation — `pageBuckets`
//     returns null and the caller renders the flat list as always

import type { SettingsGroup, SettingsSchema } from '@sdk';

/**
 * Bucket id of the implicit page collecting unassigned groups. The leading
 * underscores keep it outside the authored-id grammar (ids must start with
 * [A-Za-z0-9]), so a schema cannot declare a page that collides with it.
 */
export const GENERAL_PAGE_ID = '__general';

export interface PageBucket {
  id: string;
  /** English tab label; '' on the implicit page — the renderer translates its default. */
  label: string;
  /**
   * This page's groups with their ORIGINAL schema index. The index is what
   * collapse identity and the search jump are keyed on; a per-page index
   * would alias groups across pages.
   */
  groups: Array<{ group: SettingsGroup; index: number }>;
}

/**
 * A declared page that can actually be referenced: an object whose id matches
 * the authored-id grammar (leading [A-Za-z0-9] — the same first-character rule
 * every other authored id uses). Enforcing it here is what makes the
 * GENERAL_PAGE_ID comment above true: an underscore-leading id like
 * "__general" cannot be declared, so it can never alias the implicit bucket.
 */
function declaredPages(schema: SettingsSchema): Array<{ id: string; label: string }> {
  const raw = (schema as { pages?: unknown }).pages;
  if (!Array.isArray(raw)) return [];
  const seen = new Set<string>();
  const out: Array<{ id: string; label: string }> = [];
  for (const p of raw) {
    const page = p as { id?: unknown; label?: unknown } | null;
    if (!page || typeof page !== 'object') continue;
    if (typeof page.id !== 'string' || !/^[A-Za-z0-9]/.test(page.id) || seen.has(page.id)) continue;
    seen.add(page.id);
    out.push({ id: page.id, label: typeof page.label === 'string' && page.label ? page.label : page.id });
  }
  return out;
}

/**
 * The mod's page tabs in paint order, or null when the schema declares no
 * usable segmentation (no pages, all-empty pages, or everything on one page).
 */
export function pageBuckets(schema: SettingsSchema | undefined): PageBucket[] | null {
  if (!schema) return null;
  const declared = declaredPages(schema);
  if (!declared.length) return null;

  const general: PageBucket = { id: GENERAL_PAGE_ID, label: '', groups: [] };
  const byId = new Map<string, PageBucket>();
  const buckets: PageBucket[] = [general];
  for (const p of declared) {
    const bucket: PageBucket = { id: p.id, label: p.label, groups: [] };
    byId.set(p.id, bucket);
    buckets.push(bucket);
  }

  (schema.groups || []).forEach((group, index) => {
    const pid = (group as { page?: unknown }).page;
    const bucket = (typeof pid === 'string' && byId.get(pid)) || general;
    bucket.groups.push({ group, index });
  });

  const nonEmpty = buckets.filter((b) => b.groups.length > 0);
  return nonEmpty.length >= 2 ? nonEmpty : null;
}

/**
 * The page bucket a group (by schema index) renders under, or null when the
 * mod is unpaged. What a search jump uses to raise the right tab first.
 */
export function pageIdForGroup(schema: SettingsSchema | undefined, groupIndex: number): string | null {
  const buckets = pageBuckets(schema);
  if (!buckets) return null;
  for (const b of buckets) {
    if (b.groups.some((g) => g.index === groupIndex)) return b.id;
  }
  return null;
}
