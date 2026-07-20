// Image/icon path sandbox. Schema-declared asset paths (`type:"image"` rows and
// the rail `icon`) are untrusted author text destined for an <img src>, so they
// are confined to the mod's own `views/<modId>/` folder first.
//
// The overlapping rejection rules are belt-and-braces. Percent-encoding is
// rejected outright rather than decoded and re-checked: WebKit resolves the URL
// a second time after this runs and would turn a surviving "%2e%2e%2f" back
// into "../".

/**
 * Optional mod-id -> asset-root overrides. A parameter, not a global: production
 * never passes one, so the shipped path cannot be redirected by anything that
 * sets a global. The dev harness passes its map at the call site because it
 * serves the page from devtools/harness/ where "../../<modId>" is false.
 */
export type AssetRoots = Record<string, string>;

/**
 * In game every view mounts under one `views/` root and this page lives two
 * levels deep (views/osfui/settings/), so a mod's namespace folder is always
 * exactly two levels up.
 */
export const DEFAULT_ASSET_ROOT = '../..';

/** Scheme-ish prefix ("http:", "javascript:", "data:", "FILE:"). */
const SCHEME_RE = /^[a-z]+:/i;

/**
 * Traversal / absolute / scheme test, applied to the mod id, the source path and
 * the decoded source path.
 *
 * `includes("..")` is broader than a path-segment check — it also rejects
 * "a..b", which no legitimate asset name needs — in exchange for never having to
 * reason about separator normalisation.
 */
function isBadPath(v: string): boolean {
  return v.includes('..') || SCHEME_RE.test(v) || v.startsWith('/') || v.startsWith('\\');
}

/**
 * Resolve a schema-declared asset path to a URL, or `null` when it is rejected.
 *
 * Rejects in this order (a malformed escape short-circuits before the mod id is
 * looked at):
 *  1. an empty/absent `src`;
 *  2. a `src` whose percent-escapes do not decode (`decodeURIComponent` throws);
 *  3. an empty mod id, a mod id containing "%", or a mod id failing `isBadPath` —
 *     the id is interpolated into the path too, and while the store sanitises
 *     real ids this renderer also runs against mock data;
 *  4. a `src` containing "%" at all, or failing `isBadPath` raw or decoded.
 */
export function safeAssetSrc(
  modId: unknown,
  src: unknown,
  roots?: AssetRoots,
): string | null {
  const s = String(src || '');
  if (!s) return null;

  let decoded = s;
  try {
    decoded = decodeURIComponent(s);
  } catch {
    // Malformed escape ("%zz", a lone "%"). Reject rather than guess.
    return null;
  }

  const id = String(modId || '');
  if (!id || id.includes('%') || isBadPath(id)) return null;

  if (s.includes('%') || isBadPath(s) || isBadPath(decoded)) return null;

  const override = roots ? roots[id] : undefined;
  const root = typeof override === 'string' ? override : DEFAULT_ASSET_ROOT;
  // Interpolate the raw `s`, not `decoded` — the decode was only a check. With
  // "%" already rejected the two are identical; using `s` keeps that true if the
  // "%" rule is ever relaxed.
  return `${root}/${id}/${s}`;
}

/**
 * Rail avatar / detail icon path: a mod's schema `icon`, held to the same
 * sandbox. Non-string or absent icons yield null so the caller falls back to
 * title initials.
 */
export function modIconSrc(
  mod: { id: string; schema?: { icon?: unknown } | undefined } | null | undefined,
  roots?: AssetRoots,
): string | null {
  const icon = mod && mod.schema ? mod.schema.icon : null;
  if (!mod || typeof icon !== 'string') return null;
  return safeAssetSrc(mod.id, icon, roots);
}
