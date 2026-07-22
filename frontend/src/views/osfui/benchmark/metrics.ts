export interface FrameSummary {
  fps: number;
  frameP50: number;
  frameP95: number;
  frameP99: number;
  workP95: number;
  slowFrames: number;
  sampleCount: number;
}

export function percentile(values: readonly number[], fraction: number): number {
  if (!values.length) return 0;
  const sorted = [...values].sort((a, b) => a - b);
  const clamped = Math.max(0, Math.min(1, fraction));
  return sorted[Math.min(sorted.length - 1, Math.floor((sorted.length - 1) * clamped))]!;
}

export function summarizeFrames(
  frameGaps: readonly number[],
  workTimes: readonly number[],
): FrameSummary {
  const total = frameGaps.reduce((sum, value) => sum + value, 0);
  return {
    fps: total > 0 ? (frameGaps.length * 1000) / total : 0,
    frameP50: percentile(frameGaps, 0.5),
    frameP95: percentile(frameGaps, 0.95),
    frameP99: percentile(frameGaps, 0.99),
    workP95: percentile(workTimes, 0.95),
    slowFrames: frameGaps.filter((gap) => gap > 25).length,
    sampleCount: frameGaps.length,
  };
}
