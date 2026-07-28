// The harness stage fit maths. Dev-tool code, but the fill mode has to model
// what the runtime does to a view (pin the scale to 900 reference rows, let the
// width follow the output aspect) or it teaches the wrong layout.

import { describe, expect, it } from 'vitest';

import { BAR_H, STAGE_H, STAGE_W, computeFit, nextStageMode } from '../harness/Stage';

describe('computeFit', () => {
  it('letterboxes the fixed frame and centres it', () => {
    // Wider than 16:9 once the bar is taken off: height binds, bars at the sides.
    const fit = computeFit(2400, 900 + BAR_H, 'fixed');
    expect(fit.scale).toBe(1);
    expect(fit.width).toBe(STAGE_W);
    expect(fit.height).toBe(STAGE_H);
    expect(fit.left).toBe((2400 - STAGE_W) / 2);
    expect(fit.top).toBe(BAR_H);
  });

  it('does not cap the fixed frame at 1:1 — 1.2x in a 1080p window is the in-game text size', () => {
    expect(computeFit(1920, 1080 + BAR_H, 'fixed').scale).toBe(1.2);
  });

  it('fills the window in fill mode, scale pinned to the 900-row height', () => {
    const fit = computeFit(2400, 900 + BAR_H, 'fill');
    expect(fit.scale).toBe(1);
    expect(fit.height).toBe(STAGE_H);
    expect(fit.width).toBe(2400);
    expect(fit.left).toBe(0);
    expect(fit.top).toBe(BAR_H);
  });

  it('keeps the fill stage exactly covering the window at any aspect', () => {
    for (const [w, h] of [
      [1280, 800],
      [1920, 1080],
      [3440, 1440],
      [800, 1200],
    ]) {
      const fit = computeFit(w!, h!, 'fill');
      expect(fit.scale).toBeCloseTo((h! - BAR_H) / STAGE_H, 10);
      expect(fit.width * fit.scale).toBeCloseTo(w!, 6);
      expect(fit.height * fit.scale).toBeCloseTo(h! - BAR_H, 6);
    }
  });

  it('survives a window shorter than the toolbar without a zero or negative scale', () => {
    for (const mode of ['fixed', 'fill'] as const) {
      const fit = computeFit(200, 10, mode);
      expect(fit.scale).toBeGreaterThan(0);
      expect(Number.isFinite(fit.scale)).toBe(true);
    }
  });
});

describe('nextStageMode', () => {
  it('cycles fixed -> fill -> off -> fixed', () => {
    expect(nextStageMode('fixed')).toBe('fill');
    expect(nextStageMode('fill')).toBe('off');
    expect(nextStageMode('off')).toBe('fixed');
  });
});
