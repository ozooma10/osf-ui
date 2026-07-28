// The game-true reference frame. Dev only.
//
// Every built-in view manifest declares 1600x900 as its initial size; the game
// then resizes the page to the output aspect, so this is the reference
// composition, not the only in-game resolution. Two modes:
//
//   'fill' — 900 reference rows tall, as many CSS pixels wide as the window's
//            aspect gives. This is what the game actually does: the UI scale
//            stays pinned to the 900p row height and the page just gets wider
//            or narrower. The authoring baseline.
//   'off'  — no stage at all; the view reflows to the raw browser window,
//            unscaled. For inspecting overflow and measuring in DevTools
//            without a transform in the way, not for authoring layout.
//
// There was also a letterboxed 'fixed' mode that rendered the literal 1600x900
// box. It was dropped: at a 16:9 window it is 'fill' with bars, and at any
// other aspect it shows a composition the game never produces.
//
// 'fill' is uniformly scaled with no upscale cap — filling a 1080p window at
// 1.2x is the in-game text size, and capping at 1:1 would render everything
// smaller here than in game. The scale transform also makes the stage the
// containing block for the view's `position: fixed` scrim and toast stack,
// keeping them inside the frame instead of escaping to the browser viewport.

import type { ComponentChildren } from 'preact';
import { useLayoutEffect, useState } from 'preact/hooks';

/** The reference row height every built-in manifest declares; pins the scale. */
export const STAGE_H = 900;
/** Height of the harness toolbar; the stage is offset below it. */
export const BAR_H = 30;

/** `?res=` values, in the order the toolbar button cycles them. */
export const STAGE_MODES = ['fill', 'off'] as const;
export type StageMode = (typeof STAGE_MODES)[number];

/** The mode the toolbar button switches to next. */
export function nextStageMode(mode: StageMode): StageMode {
  return STAGE_MODES[(STAGE_MODES.indexOf(mode) + 1) % STAGE_MODES.length] ?? 'fill';
}

export interface StageFit {
  scale: number;
  left: number;
  top: number;
  /** Stage size in CSS pixels, before `scale`. */
  width: number;
  height: number;
}

/** Pure, so the fit maths is testable without a DOM. */
export function computeFit(winW: number, winH: number, barH = BAR_H): StageFit {
  // Height fixes the scale (900 reference rows, as in game); width is whatever
  // is left over, so the stage covers the window exactly.
  const availH = Math.max(1, winH - barH);
  const scale = availH / STAGE_H;
  return { scale, left: 0, top: barH, width: Math.max(1, winW) / scale, height: STAGE_H };
}

export function Stage({ mode, children }: { mode: StageMode; children: ComponentChildren }) {
  const [fit, setFit] = useState<StageFit>(() => computeFit(window.innerWidth, window.innerHeight));

  useLayoutEffect(() => {
    if (mode === 'off') return;
    const onResize = () => setFit(computeFit(window.innerWidth, window.innerHeight));
    onResize();
    window.addEventListener('resize', onResize);
    return () => window.removeEventListener('resize', onResize);
  }, [mode]);

  // Unscaled mode (?res=off): no transform, no fixed size. For inspecting
  // overflow, not for authoring layout.
  if (mode === 'off') return <div id="stage">{children}</div>;

  return (
    <div
      id="stage"
      style={{
        position: 'fixed',
        width: `${fit.width}px`,
        height: `${fit.height}px`,
        transformOrigin: '0 0',
        overflow: 'hidden',
        transform: `scale(${fit.scale})`,
        left: `${fit.left}px`,
        top: `${fit.top}px`,
        outline: '1px dashed var(--osf-line-strong)',
        background: 'radial-gradient(120% 100% at 50% 30%, #10161f 0%, #05070b 100%)',
      }}
    >
      {children}
    </div>
  );
}
