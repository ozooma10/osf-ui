
import type { SettingsGroup, SettingsSchema } from '@sdk';

export const GENERAL_PAGE_ID = '__general';

export interface PageBucket {
  id: string;
  /** English tab label; '' on the implicit page — the renderer translates its default. */
  label: string;
  groups: Array<{ group: SettingsGroup; index: number }>;
}

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

export function pageIdForGroup(schema: SettingsSchema | undefined, groupIndex: number): string | null {
  const buckets = pageBuckets(schema);
  if (!buckets) return null;
  for (const b of buckets) {
    if (b.groups.some((g) => g.index === groupIndex)) return b.id;
  }
  return null;
}
