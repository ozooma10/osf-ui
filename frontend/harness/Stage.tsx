// Stage.tsx — the game-true 1600x900 reference frame. DEV ONLY.
//
// Every built-in view manifest declares width 1600 / height 900 as its initial
// size. The game later resizes the page to the output aspect, so this stage is
// the reference composition rather than the only in-game resolution.
//
// The harness provides a fixed 1600x900 box, uniformly
// scaled to fill the window. There is deliberately NO upscale cap - filling a
// 1080p window at 1.2x IS the in-game text size, and capping at 1:1 would make
// everything look smaller here than it does in game.
//
// The scale transform matters for a second reason: it makes the stage the
// containing block for the view's `position: fixed` scrim and toast stack, so
// those stay inside the 900p frame instead of escaping to the browser viewport.

import type { ComponentChildren } from 'preact';
import { useLayoutEffect, useState } from 'preact/hooks';

export const STAGE_W = 1600;
export const STAGE_H = 900;
/** Height of the harness toolbar; the stage is offset below it. */
export const BAR_H = 30;

export interface StageFit {
  scale: number;
  left: number;
  top: number;
}

/** Pure, so the fit maths is unit-testable without a DOM. */
export function computeFit(winW: number, winH: number, barH = BAR_H): StageFit {
  const scale = Math.min(winW / STAGE_W, (winH - barH) / STAGE_H);
  return {
    scale,
    left: Math.max(0, (winW - STAGE_W * scale) / 2),
    top: barH + Math.max(0, (winH - barH - STAGE_H * scale) / 2),
  };
}

export function Stage({ enabled, children }: { enabled: boolean; children: ComponentChildren }) {
  const [fit, setFit] = useState<StageFit>(() => computeFit(window.innerWidth, window.innerHeight));

  useLayoutEffect(() => {
    if (!enabled) return;
    const onResize = () => setFit(computeFit(window.innerWidth, window.innerHeight));
    onResize();
    window.addEventListener('resize', onResize);
    return () => window.removeEventListener('resize', onResize);
  }, [enabled]);

  // Fluid mode (?res=off): no transform, no fixed size. Useful for inspecting
  // overflow, NOT for authoring layout.
  if (!enabled) return <div id="stage">{children}</div>;

  return (
    <div
      id="stage"
      style={{
        position: 'fixed',
        width: `${STAGE_W}px`,
        height: `${STAGE_H}px`,
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
