
import type { ComponentChildren, JSX } from 'preact';

export interface OverlayProps {
  children: ComponentChildren;
  class: string;
  onClick?: JSX.MouseEventHandler<HTMLDivElement>;
}

export function Overlay({ children, class: className, onClick }: OverlayProps) {
  return (
    <div class={className} data-nav-modal="1" {...(onClick ? { onClick } : {})}>
      {children}
    </div>
  );
}
