// Identity glyphs shared by the rail and the Home launcher: title initials, the
// derived card accent, and the icon-with-fallback avatar (three states: schema
// icon, broken icon, no icon).
//
// The Home patch/monogram variant keeps its own copy in Home.tsx because its
// fallback sits inside an SVG frame rather than replacing the whole node.

import { useState } from 'preact/hooks';
import type { ComponentChildren } from 'preact';

/**
 * Up to two letters standing in for a title. Two words -> their initials
 * ("Ship Almanac" -> "SA"); one word -> its first two alphanumeric characters,
 * so "acme.shipworks" reads "AC" without the stray dot.
 *
 * The branches are asymmetric: the two-word path does not strip punctuation, so
 * ".x y" yields ".Y" — unreachable for any title the schema validator accepts.
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
 * Home card palette. Muted and not the kit accent: these tint unbranded
 * third-party views and must read as assigned automatically, not as a mod's
 * own colour choice.
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
 * djb2-ish string hash; `>>> 0` keeps it unsigned 32-bit. Only stability
 * matters: the same view id must pick the same colour across sessions, so no
 * randomness and no ordering dependence.
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
  /** Added only while a real icon is showing, e.g. "rail-item-mark--icon". */
  iconClass: string;
  /** Already through safeAssetSrc; null when the mod ships none / it was rejected. */
  src: string | null;
  /** Inline colour, or "" for none (the Home chip tints itself, the rail does not). */
  color: string;
  /** Shown when there is no icon, or once the icon fails to load. */
  fallback: ComponentChildren;
}

/**
 * A mod's schema `icon` when it has one, its initials otherwise. The `onError`
 * fallback matters: a schema can name a file that was removed, renamed or never
 * shipped, and a stale path must leave initials rather than a broken-image hole.
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
