// Overlay.tsx — a modal layer that TRAPS gamepad/arrow navigation.
//
// The second padnav contract (src/legacy/padnav.js:75-77, 211-212):
//
//   function navRoot() {
//     return document.querySelector("[data-nav-modal]") || document;
//   }
//
// While any element carrying `data-nav-modal` is in the document, padnav
// enumerates candidates only inside it, and drops an `active` element that sits
// outside it. So the attribute IS the focus trap — there is no other mechanism.
//
// The settings view's undo panel is the only current user
// (settings/main.legacy.js:1505: `overlay.dataset.navModal = "1"` on
// `.session-overlay`). It lives here because the value is load-bearing in a
// file this migration ships verbatim, and because a second modal must not
// re-derive it.
//
// NOTE the selector is `[data-nav-modal]`, attribute-presence, not
// `[data-nav-modal="1"]` — padnav does not read the value. "1" is written
// anyway to stay byte-identical to the shipped markup.

import type { ComponentChildren, JSX } from 'preact';

export interface OverlayProps {
  children: ComponentChildren;
  /** The overlay's own class, e.g. "session-overlay". */
  class: string;
  /**
   * Optional backdrop handler. The settings undo panel uses it for
   * click-outside-to-close and tests `e.target === e.currentTarget`, so a click
   * that lands on the panel bubbles up here WITHOUT matching and the panel
   * stays put (settings/main.legacy.js:1530). Attaching it to this node rather
   * than to a synthetic sibling is what makes that identity test meaningful.
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
