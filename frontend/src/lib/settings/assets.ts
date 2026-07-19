// assets.ts — the image/icon path sandbox (main.legacy.js:653-673).
//
// A schema may point at an image (`type:"image"` rows, and the rail `icon`).
// The path is UNTRUSTED author text that ends up in an <img src>, so it is
// confined to the mod's own `views/<modId>/` folder before it goes anywhere
// near the DOM.
//
// The rejection set is belt-and-braces on purpose — several rules overlap, and
// each is cheap. Percent-encoding is rejected OUTRIGHT rather than decoded and
// re-checked, because WebKit resolves the URL a second time after this function
// runs and would turn a surviving "%2e%2e%2f" back into "../".

/**
 * Optional mod-id -> asset-root overrides.
 *
 * The legacy version read `window.OSFUI_MOD_ASSET_ROOTS`, a global the dev
 * harness defines because it serves this page from devtools/harness/ where the
 * "../../<modId>" assumption is false. This port takes it as a PARAMETER
 * instead: production code never passes one, so the shipped path cannot be
 * redirected by anything that manages to set a global, and the harness injects
 * its map explicitly at the call site.
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
 * Traversal / absolute / scheme test, applied to the mod id AND to the source
 * path AND to the decoded source path.
 *
 * `includes("..")` is intentionally broader than a path-segment check: it also
 * rejects "a..b", which no legitimate asset name needs, in exchange for never
 * having to reason about separator normalisation.
 */
function isBadPath(v: string): boolean {
  return v.includes('..') || SCHEME_RE.test(v) || v.startsWith('/') || v.startsWith('\\');
}

/**
 * Resolve a schema-declared asset path to a URL, or `null` when it is rejected.
 *
 * Rejects, in this order (order matters — a malformed escape short-circuits
 * before the mod id is even looked at):
 *  1. an empty/absent `src`;
 *  2. a `src` whose percent-escapes do not decode (`decodeURIComponent` throws);
 *  3. an empty mod id, a mod id containing "%", or a mod id that fails
 *     `isBadPath` — the id is interpolated into the path too, and while the
 *     store sanitises real ids this renderer also runs against mock data;
 *  4. a `src` containing "%" at all, or failing `isBadPath` raw OR decoded.
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
  // The RAW `s` is interpolated, not `decoded` — the decode was only ever a
  // check. (With "%" already rejected the two are identical anyway; using `s`
  // keeps that true by construction if the "%" rule is ever relaxed.)
  return `${root}/${id}/${s}`;
}

/**
 * The rail avatar / detail icon path: a mod's schema `icon`, held to the same
 * sandbox. Non-string icons (or none) yield null so the caller falls back to
 * title initials. main.legacy.js:801-804.
 */
export function modIconSrc(
  mod: { id: string; schema?: { icon?: unknown } | undefined } | null | undefined,
  roots?: AssetRoots,
): string | null {
  const icon = mod && mod.schema ? mod.schema.icon : null;
  if (!mod || typeof icon !== 'string') return null;
  return safeAssetSrc(mod.id, icon, roots);
}
