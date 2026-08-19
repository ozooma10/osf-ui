
export type AssetRoots = Record<string, string>;

export const DEFAULT_ASSET_ROOT = '../..';

/** Scheme-ish prefix ("http:", "javascript:", "data:", "FILE:"). */
const SCHEME_RE = /^[a-z]+:/i;

function isBadPath(v: string): boolean {
  return v.includes('..') || SCHEME_RE.test(v) || v.startsWith('/') || v.startsWith('\\');
}

/** Mirrors the native mod-id path boundary for untrusted mock payloads. */
function isBadModId(v: string): boolean {
  if (!v || new TextEncoder().encode(v).byteLength > 64 || v === '.' || v === '..' || /[. ]$/.test(v) ||
      /[\u0000-\u001f<>:"/\\|?*#%]/.test(v)) return true;
  if (v.toLowerCase() === 'osfui') return v !== 'osfui';
  return /^(?:con|prn|aux|nul|com[1-9]|lpt[1-9])(?:\.|$)/i.test(v);
}

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
  if (isBadModId(id)) return null;

  if (s.includes('%') || isBadPath(s) || isBadPath(decoded)) return null;

  const override = roots ? roots[id] : undefined;
  const root = typeof override === 'string' ? override : DEFAULT_ASSET_ROOT;
  return `${root}/${id}/${s}`;
}

export function modIconSrc(
  mod: { id: string; schema?: { icon?: unknown } | undefined } | null | undefined,
  roots?: AssetRoots,
): string | null {
  const icon = mod && mod.schema ? mod.schema.icon : null;
  if (!mod || typeof icon !== 'string') return null;
  return safeAssetSrc(mod.id, icon, roots);
}
