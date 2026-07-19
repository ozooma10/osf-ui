// marks.tsx — the little identity glyphs shared by the rail and the Home
// launcher: title initials, the derived card accent, and the icon-with-fallback
// avatar.
//
// Extracted rather than duplicated because the rail (main.legacy.js:813-829),
// the Home HUD chip (:1270-1287) and the Home card monogram (:1222-1235) all
// implement the SAME three-state widget — schema icon, broken icon, no icon —
// and legacy had it written out three times. The patch/monogram variant keeps
// its own copy in Home.tsx because its fallback sits inside an SVG frame rather
// than replacing the whole node.

import { useState } from 'preact/hooks';
import type { ComponentChildren } from 'preact';

/**
 * Up to two letters standing in for a title (main.legacy.js:181-185).
 *
 * Two words -> their initials ("Ship Almanac" -> "SA"). One word -> its first
 * two ALPHANUMERIC characters, so "acme.shipworks" reads "AC" rather than
 * "AC" with a stray dot. Note the two branches are asymmetric: the two-word
 * path does NOT strip punctuation, so ".x y" yields ".Y" — faithful, and
 * unreachable for any title the schema validator accepts.
 */
export function initials(title: unknown): string {
  const words = String(title).trim().split(/\s+/);
  const first = words[0] || '';
  if (words.length >= 2) {
    const second = words[1] || '';
    return ((first[0] || '') + (second[0] || '')).toUpperCase();
  }
  return first.replace(/[^A-Za-z0-9]/g, '').slice(0, 2).toUpperCase();
}

/**
 * The Home card palette (main.legacy.js:1172). Muted, deliberately NOT the kit
 * accent: these tint unbranded third-party views, and they must read as
 * "assigned automatically", not as "this mod chose teal".
 */
export const HOME_PALETTE = [
  '#6f93b0',
  '#7a9a5e',
  '#c98a4a',
  '#b96f86',
  '#8b83c0',
  '#b9a45e',
  '#5f9aa0',
  '#a8846a',
] as const;

/**
 * djb2-ish string hash, `>>> 0` to stay in unsigned 32-bit
 * (main.legacy.js:1173-1177). Only stability matters — the same view id must
 * pick the same colour across sessions, so no randomness and no ordering
 * dependence.
 */
export function hashId(id: string): number {
  let h = 0;
  for (let i = 0; i < id.length; i++) h = (h * 31 + id.charCodeAt(i)) >>> 0;
  return h;
}

export function homeAccentFor(id: unknown): string {
  return HOME_PALETTE[hashId(String(id)) % HOME_PALETTE.length] as string;
}

export interface MarkProps {
  /** Base class, e.g. "rail-item-mark". */
  class: string;
  /** Added ONLY while a real icon is showing, e.g. "rail-item-mark--icon". */
  iconClass: string;
  /** Already through safeAssetSrc; null when the mod ships none / it was rejected. */
  src: string | null;
  /** Inline colour, or "" for none (the Home chip tints itself, the rail does not). */
  color: string;
  /** Shown when there is no icon, or once the icon fails to load. */
  fallback: ComponentChildren;
}

/**
 * A mod's schema `icon` when it has one, its initials otherwise.
 *
 * The `onError` fallback is the point: a schema can name a file that was
 * removed, renamed, or never shipped, and a stale path must leave initials —
 * not a broken-image hole in the middle of the rail.
 */
export function Mark({ class: base, iconClass, src, color, fallback }: MarkProps) {
  const [failed, setFailed] = useState(false);
  const showIcon = !!src && !failed;
  return (
    <span
      class={showIcon ? `${base} ${iconClass}` : base}
      {...(color ? { style: { color } } : {})}
    >
      {showIcon ? (
        // alt="" — the mark is decorative; the title next to it is the name.
        <img src={src as string} alt="" onError={() => setFailed(true)} />
      ) : (
        fallback
      )}
    </span>
  );
}
