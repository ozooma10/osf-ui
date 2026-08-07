// Stage fit maths for the authoring harness. Pure, so it is testable under
// node --test without a DOM.
//
// Views declare an initial size (manifest width/height); in game the runtime
// resizes the page to the output aspect, keeping the reference row height.
// Two modes:
//
//   'fill' — the declared height's worth of reference rows, as many CSS
//            pixels wide as the pane's aspect gives. This is what the game
//            actually does: the UI scale stays fixed to the reference row
//            height and the page just gets wider or narrower.
//   'off'  — no stage: the view reflows to the raw pane, unscaled. For
//            inspecting overflow, not for authoring layout.
//
// There was also a letterboxed 'fixed' mode that rendered the literal declared
// box. It was dropped: at a 16:9 pane it is 'fill' with bars, and at any other
// aspect it shows a composition the game never produces.
//
// 'fill' does not cap the scale at 1:1 — filling a 1080p pane with a 900-row
// view at 1.2x IS the in-game text size; capping would render everything
// smaller in the harness than in game.

export const STAGE_MODES = ['fill', 'off'];

/** The mode the toolbar button switches to next. */
export function nextStageMode(mode) {
  const index = STAGE_MODES.indexOf(mode);
  return STAGE_MODES[(index + 1) % STAGE_MODES.length] ?? 'fill';
}

/**
 * Fit a view's stageH reference rows into an availW x availH pane. Returns the
 * pre-scale stage size in CSS pixels plus the uniform scale; 'off' is the
 * caller's branch (no frame, no maths). The view's declared width does not
 * enter into it — in fill mode the width follows the pane.
 */
export function computeFit(availW, availH, stageH) {
  const paneW = Math.max(1, availW);
  const paneH = Math.max(1, availH);
  const frameH = Math.max(1, stageH);
  // Height fixes the scale (reference rows, as in game); width is whatever is
  // left over, so the stage covers the pane exactly.
  const scale = paneH / frameH;
  return { scale, width: paneW / scale, height: frameH };
}
