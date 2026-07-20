// Fixed backdrop that dims the live game behind a view. No content, no handlers:
// it is all CSS (`position: fixed; inset: 0; pointer-events: none`), styled
// per-view since each view tunes its own gradient. `aria-hidden` because it is
// decoration — a screen reader should land straight on the panel.

export function Scrim() {
  return <div class="scrim" aria-hidden="true" />;
}
