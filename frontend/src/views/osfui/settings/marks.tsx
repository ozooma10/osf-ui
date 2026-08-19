
import { useState } from 'preact/hooks';
import type { ComponentChildren } from 'preact';

export function initials(title: unknown): string {
  const words = String(title).trim().split(/\s+/);
  const first = words[0] || '';
  if (words.length >= 2) {
    const second = words[1] || '';
    return ((first[0] || '') + (second[0] || '')).toUpperCase();
  }
  return first.replace(/[^A-Za-z0-9]/g, '').slice(0, 2).toUpperCase();
}

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
