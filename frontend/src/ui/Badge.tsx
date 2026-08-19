
import type { ComponentChildren } from 'preact';
import { cx } from './cx';
import { optAttr } from './optAttr';

export interface BadgeProps {
  children: ComponentChildren;
  modifier: string;
  /** Tooltip. "" omits the attribute rather than emitting an empty one. */
  title: string;
}

export function Badge({ children, modifier, title }: BadgeProps) {
  return (
    <span class={cx('osf-badge', modifier)} {...optAttr('title', title)}>
      {children}
    </span>
  );
}
