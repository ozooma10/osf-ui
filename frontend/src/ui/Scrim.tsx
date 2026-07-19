// Scrim.tsx — the fixed backdrop that dims the live game behind a view.
//
// Both first-party views open with the identical node
// (keybinds/index.html:12, settings/index.html:12):
//
//   <div class="scrim" aria-hidden="true"></div>
//
// It carries no content and no handlers — every pixel of it is CSS
// (`position: fixed; inset: 0; pointer-events: none`), styled per-view because
// each view tunes its own gradient. `aria-hidden` because it is pure
// decoration: a screen reader must walk straight into the panel.

export function Scrim() {
  return <div class="scrim" aria-hidden="true" />;
}
