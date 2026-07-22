import { describe, expect, it } from 'vitest';
import { percentile, summarizeFrames } from '../src/views/osfui/benchmark/metrics';

describe('benchmark metrics', () => {
  it('calculates stable nearest-rank percentiles without mutating samples', () => {
    const values = [30, 10, 20, 40];
    expect(percentile(values, 0.5)).toBe(20);
    expect(percentile(values, 0.95)).toBe(30);
    expect(values).toEqual([30, 10, 20, 40]);
  });

  it('summarizes cadence, page work and visibly slow frames', () => {
    const result = summarizeFrames([10, 10, 20, 30], [1, 2, 3, 4]);
    expect(result.fps).toBeCloseTo(57.14, 1);
    expect(result.frameP50).toBe(10);
    expect(result.frameP95).toBe(20);
    expect(result.frameP99).toBe(20);
    expect(result.workP95).toBe(3);
    expect(result.slowFrames).toBe(1);
    expect(result.sampleCount).toBe(4);
  });

  it('returns zeroes for an empty run', () => {
    expect(summarizeFrames([], [])).toEqual({
      fps: 0,
      frameP50: 0,
      frameP95: 0,
      frameP99: 0,
      workP95: 0,
      slowFrames: 0,
      sampleCount: 0,
    });
  });
});
