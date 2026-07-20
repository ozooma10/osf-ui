// Modal layer that traps gamepad/arrow navigation.
//
// While any element carrying `data-nav-modal` is in the document, padnav
// enumerates candidates only inside it and drops an `active` element outside
// it. The attribute is the only focus-trap mechanism.
//
// padnav matches on attribute presence (`[data-nav-modal]`), not on the value.
// "1" is written to stay byte-identical to the shipped markup.

import type { ComponentChildren, JSX } from 'preact';

export interface OverlayProps {
  children: ComponentChildren;
  class: string;
  /**
   * Backdrop handler. Callers implement click-outside-to-close by testing
   * `e.target === e.currentTarget`: a click landing on the panel bubbles here
   * without matching, so the panel stays put. That identity test only works
   * because the handler is on this node rather than a sibling.
   */
  onClick?: JSX.MouseEventHandler<HTMLDivElement>;
}

export function Overlay({ children, class: className, onClick }: OverlayProps) {
  return (
    <div class={className} data-nav-modal="1" {...(onClick ? { onClick } : {})}>
      {children}
    </div>
  );
}
