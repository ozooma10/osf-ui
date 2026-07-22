import { useEffect, useRef, useState } from 'preact/hooks';
import { windowBridge, type Bridge } from '@lib/bridge';
import { percentile, summarizeFrames, type FrameSummary } from './metrics';

type WorkloadId = 'pulse' | 'composite' | 'layout' | 'canvas' | 'effects' | 'mixed';

interface Workload {
  id: WorkloadId;
  name: string;
  detail: string;
}

const WORKLOADS: readonly Workload[] = [
  { id: 'pulse', name: 'Paint pulse', detail: 'One moving element; establishes the delivery baseline.' },
  { id: 'composite', name: 'Transforms', detail: 'Many animated transform and opacity layers.' },
  { id: 'layout', name: 'DOM layout', detail: 'Style writes followed by forced geometry reads.' },
  { id: 'canvas', name: 'Canvas 2D', detail: 'Animated particles, fills, paths and alpha blending.' },
  { id: 'effects', name: 'CSS effects', detail: 'Gradients, blur, shadows and translucent surfaces.' },
  { id: 'mixed', name: 'Mixed scene', detail: 'A balanced combination representative of a game UI.' },
];

const INTENSITIES = [1, 2, 4] as const;
const WARMUP_MS = 750;
const SUITE_RUN_MS = 5000;

interface ActiveRun {
  workload: WorkloadId;
  intensity: number;
  started: number;
  lastFrame: number;
  lastUi: number;
  frameGaps: number[];
  workTimes: number[];
  timerJitter: number[];
  lastTimer: number;
  timerId: number;
}

interface BenchmarkResult extends FrameSummary {
  workload: WorkloadId;
  name: string;
  intensity: number;
  durationMs: number;
  timerP95: number;
}

interface LiveResult extends FrameSummary {
  timerP95: number;
}

const EMPTY_LIVE: LiveResult = {
  fps: 0,
  frameP50: 0,
  frameP95: 0,
  frameP99: 0,
  workP95: 0,
  slowFrames: 0,
  sampleCount: 0,
  timerP95: 0,
};

function workloadName(id: WorkloadId): string {
  return WORKLOADS.find((workload) => workload.id === id)?.name || id;
}

function canvasExercise(canvas: HTMLCanvasElement, intensity: number, now: number): void {
  const width = Math.max(1, Math.floor(canvas.clientWidth * devicePixelRatio));
  const height = Math.max(1, Math.floor(canvas.clientHeight * devicePixelRatio));
  if (canvas.width !== width || canvas.height !== height) {
    canvas.width = width;
    canvas.height = height;
  }
  const context = canvas.getContext('2d', { alpha: true });
  if (!context) return;
  context.clearRect(0, 0, width, height);
  context.save();
  context.scale(devicePixelRatio, devicePixelRatio);
  const cssWidth = width / devicePixelRatio;
  const cssHeight = height / devicePixelRatio;
  const count = 180 * intensity;
  for (let i = 0; i < count; i += 1) {
    const phase = now * 0.0015 + i * 0.37;
    const x = (i * 47 + Math.sin(phase) * 90 + cssWidth * 2) % cssWidth;
    const y = (i * 29 + Math.cos(phase * 1.13) * 65 + cssHeight * 2) % cssHeight;
    const radius = 1.5 + (i % 7) * 0.55;
    context.beginPath();
    context.fillStyle = `hsla(${185 + (i % 62)}, 82%, ${55 + (i % 20)}%, 0.56)`;
    context.arc(x, y, radius, 0, Math.PI * 2);
    context.fill();
  }
  context.restore();
}

function exercise(
  workload: WorkloadId,
  intensity: number,
  now: number,
  stage: HTMLDivElement,
  canvas: HTMLCanvasElement,
): void {
  const marker = stage.querySelector<HTMLElement>('.paint-marker');
  if (marker) {
    const x = 50 + Math.sin(now * 0.003) * 43;
    marker.style.transform = `translate3d(${x}vw, 0, 0) rotate(${now * 0.04}deg)`;
  }

  if (workload === 'composite' || workload === 'mixed') {
    const limit = workload === 'mixed' ? 24 * intensity : 48 * intensity;
    const tiles = stage.querySelectorAll<HTMLElement>('.transform-tile');
    for (let i = 0; i < Math.min(limit, tiles.length); i += 1) {
      const phase = now * 0.002 + i * 0.41;
      const x = Math.sin(phase) * (10 + (i % 5) * 3);
      const y = Math.cos(phase * 0.83) * (8 + (i % 7) * 2);
      const tile = tiles.item(i);
      tile.style.transform = `translate3d(${x}px, ${y}px, 0) rotate(${Math.sin(phase) * 8}deg)`;
      tile.style.opacity = String(0.42 + (Math.sin(phase * 1.7) + 1) * 0.24);
    }
  }

  if (workload === 'layout' || workload === 'mixed') {
    const limit = workload === 'mixed' ? 36 * intensity : 72 * intensity;
    const nodes = stage.querySelectorAll<HTMLElement>('.layout-node');
    for (let i = 0; i < Math.min(limit, nodes.length); i += 1) {
      const node = nodes.item(i);
      node.style.width = `${28 + ((i * 13 + now * 0.025) % 62)}%`;
      node.style.marginLeft = `${(i * 7 + now * 0.012) % 18}%`;
    }
    let geometry = 0;
    for (let i = 0; i < Math.min(limit, nodes.length); i += 1) geometry += nodes.item(i).offsetWidth;
    stage.dataset.layoutChecksum = String(geometry);
  }

  if (workload === 'canvas' || workload === 'mixed') {
    canvasExercise(canvas, workload === 'mixed' ? Math.max(1, intensity - 1) : intensity, now);
  } else {
    const context = canvas.getContext('2d');
    context?.clearRect(0, 0, canvas.width, canvas.height);
  }

  if (workload === 'effects' || workload === 'mixed') {
    const effects = stage.querySelectorAll<HTMLElement>('.effect-orb');
    effects.forEach((effect, i) => {
      const phase = now * 0.001 + i * 1.7;
      effect.style.transform = `translate3d(${Math.sin(phase) * 35}px, ${Math.cos(phase * 1.2) * 24}px, 0) scale(${0.9 + Math.sin(phase) * 0.12})`;
      effect.style.filter = `blur(${6 + intensity * 2 + (i % 3) * 3}px)`;
    });
    stage.style.setProperty('--bench-angle', `${(now * 0.025) % 360}deg`);
  }

  if (workload === 'mixed') {
    const text = stage.querySelectorAll<HTMLElement>('.data-cell');
    for (let i = 0; i < Math.min(text.length, 18 * intensity); i += 1) {
      text.item(i).textContent = String(Math.floor((now * (i + 3)) % 10000)).padStart(4, '0');
    }
  }
}

function wait(milliseconds: number): Promise<void> {
  return new Promise((resolve) => window.setTimeout(resolve, milliseconds));
}

export interface AppProps {
  bridge?: Bridge;
}

export function App({ bridge = windowBridge }: AppProps) {
  const stageRef = useRef<HTMLDivElement>(null);
  const canvasRef = useRef<HTMLCanvasElement>(null);
  const activeRef = useRef<ActiveRun | null>(null);
  const suiteTokenRef = useRef(0);
  const [workload, setWorkload] = useState<WorkloadId>('pulse');
  const [intensity, setIntensity] = useState<number>(1);
  const [running, setRunning] = useState(false);
  const [suiteRunning, setSuiteRunning] = useState(false);
  const [live, setLive] = useState<LiveResult>(EMPTY_LIVE);
  const [results, setResults] = useState<BenchmarkResult[]>([]);
  const [inputSamples, setInputSamples] = useState<number[]>([]);
  const [copyState, setCopyState] = useState('Copy results');

  useEffect(() => {
    let animationFrame = 0;
    const tick = (now: number) => {
      const active = activeRef.current;
      const stage = stageRef.current;
      const canvas = canvasRef.current;
      if (active && stage && canvas) {
        const frameGap = active.lastFrame ? now - active.lastFrame : 0;
        active.lastFrame = now;
        const workStart = performance.now();
        exercise(active.workload, active.intensity, now, stage, canvas);
        const workMs = performance.now() - workStart;
        if (now - active.started >= WARMUP_MS && frameGap > 0) {
          active.frameGaps.push(frameGap);
          active.workTimes.push(workMs);
        }
        if (now - active.lastUi >= 250) {
          active.lastUi = now;
          const recentGaps = active.frameGaps.slice(-180);
          const recentWork = active.workTimes.slice(-180);
          setLive({
            ...summarizeFrames(recentGaps, recentWork),
            timerP95: percentile(active.timerJitter.slice(-180), 0.95),
          });
        }
      }
      animationFrame = requestAnimationFrame(tick);
    };
    animationFrame = requestAnimationFrame(tick);
    return () => {
      cancelAnimationFrame(animationFrame);
      const active = activeRef.current;
      if (active) window.clearInterval(active.timerId);
      suiteTokenRef.current += 1;
    };
  }, []);

  const startRun = (nextWorkload = workload, nextIntensity = intensity) => {
    const old = activeRef.current;
    if (old) window.clearInterval(old.timerId);
    const now = performance.now();
    const active: ActiveRun = {
      workload: nextWorkload,
      intensity: nextIntensity,
      started: now,
      lastFrame: 0,
      lastUi: now,
      frameGaps: [],
      workTimes: [],
      timerJitter: [],
      lastTimer: now,
      timerId: 0,
    };
    active.timerId = window.setInterval(() => {
      const stamp = performance.now();
      if (stamp - active.started >= WARMUP_MS) {
        active.timerJitter.push(Math.abs(stamp - active.lastTimer - 16));
      }
      active.lastTimer = stamp;
    }, 16);
    activeRef.current = active;
    setWorkload(nextWorkload);
    setIntensity(nextIntensity);
    setLive(EMPTY_LIVE);
    setRunning(true);
  };

  const finishRun = (): BenchmarkResult | null => {
    const active = activeRef.current;
    if (!active) return null;
    window.clearInterval(active.timerId);
    activeRef.current = null;
    setRunning(false);
    const summary = summarizeFrames(active.frameGaps, active.workTimes);
    const result: BenchmarkResult = {
      workload: active.workload,
      name: workloadName(active.workload),
      intensity: active.intensity,
      durationMs: performance.now() - active.started,
      timerP95: percentile(active.timerJitter, 0.95),
      ...summary,
    };
    if (summary.sampleCount) setResults((current) => [...current, result]);
    return result;
  };

  const runSuite = async () => {
    const token = suiteTokenRef.current + 1;
    suiteTokenRef.current = token;
    finishRun();
    setResults([]);
    setSuiteRunning(true);
    for (const candidate of WORKLOADS) {
      if (suiteTokenRef.current !== token) break;
      startRun(candidate.id, intensity);
      await wait(SUITE_RUN_MS);
      if (suiteTokenRef.current !== token) break;
      finishRun();
      await wait(250);
    }
    if (suiteTokenRef.current === token) setSuiteRunning(false);
  };

  const stop = () => {
    suiteTokenRef.current += 1;
    setSuiteRunning(false);
    finishRun();
  };

  const measureInput = () => {
    const started = performance.now();
    requestAnimationFrame((paintOpportunity) => {
      setInputSamples((current) => [...current.slice(-19), paintOpportunity - started]);
    });
  };

  const copyResults = async () => {
    const payload = JSON.stringify(
      {
        generatedAt: new Date().toISOString(),
        userAgent: navigator.userAgent,
        devicePixelRatio,
        inputToRafP95: percentile(inputSamples, 0.95),
        results,
      },
      null,
      2,
    );
    try {
      await navigator.clipboard.writeText(payload);
      setCopyState('Copied');
    } catch {
      const text = document.createElement('textarea');
      text.value = payload;
      text.style.position = 'fixed';
      text.style.opacity = '0';
      document.body.appendChild(text);
      text.select();
      document.execCommand('copy');
      text.remove();
      setCopyState('Copied');
    }
    window.setTimeout(() => setCopyState('Copy results'), 1400);
  };

  const tileCount = 48 * intensity;
  const layoutCount = 72 * intensity;
  const inputP95 = percentile(inputSamples, 0.95);

  return (
    <div class="benchmark-shell">
      <div class="bench-scrim" />
      <header class="bench-header">
        <div>
          <div class="bench-eyebrow">OSF UI · DIAGNOSTICS</div>
          <h1>Web Performance Lab</h1>
          <p>Repeatable rendering workloads for comparing browser, capture and in-game delivery.</p>
        </div>
        <div class="header-actions">
          <button type="button" onClick={() => bridge.send('menu.open', { view: 'osfui/settings' })}>
            Back to Mods
          </button>
        </div>
      </header>

      <main class="bench-main">
        <section class="stage-panel">
          <div class="panel-title">
            <span>LIVE WORKLOAD</span>
            <span class={running ? 'run-state is-running' : 'run-state'}>{running ? 'RUNNING' : 'IDLE'}</span>
          </div>
          <div
            ref={stageRef}
            class={`benchmark-stage workload-${workload}`}
            onPointerDown={measureInput}
          >
            <div class="stage-grid" />
            <div class="paint-marker" />
            <canvas ref={canvasRef} class="bench-canvas" />
            <div class="transform-field">
              {Array.from({ length: tileCount }, (_, i) => <i class="transform-tile" key={i} />)}
            </div>
            <div class="layout-field">
              {Array.from({ length: layoutCount }, (_, i) => <i class="layout-node" key={i} />)}
            </div>
            <div class="effect-field">
              {Array.from({ length: 8 * intensity }, (_, i) => <i class="effect-orb" key={i} />)}
            </div>
            <div class="data-field">
              {Array.from({ length: 18 * intensity }, (_, i) => <span class="data-cell" key={i}>0000</span>)}
            </div>
            <div class="stage-caption">
              <strong>{workloadName(workload)}</strong>
              <span>{WORKLOADS.find((entry) => entry.id === workload)?.detail}</span>
            </div>
          </div>

          <div class="live-metrics">
            <Metric label="PAGE RAF" value={`${live.fps.toFixed(1)} fps`} hot={live.frameP95 > 25} />
            <Metric label="FRAME P95" value={`${live.frameP95.toFixed(2)} ms`} hot={live.frameP95 > 25} />
            <Metric label="JS WORK P95" value={`${live.workP95.toFixed(2)} ms`} hot={live.workP95 > 8} />
            <Metric label="TIMER JITTER" value={`${live.timerP95.toFixed(2)} ms`} hot={live.timerP95 > 8} />
            <Metric label="INPUT → RAF" value={inputSamples.length ? `${inputP95.toFixed(2)} ms` : 'click stage'} hot={inputP95 > 25} />
          </div>
        </section>

        <aside class="control-panel">
          <section class="control-section">
            <div class="panel-title"><span>WORKLOAD</span></div>
            <div class="workload-list">
              {WORKLOADS.map((entry) => (
                <button
                  type="button"
                  class={workload === entry.id ? 'workload-button is-active' : 'workload-button'}
                  disabled={running}
                  onClick={() => setWorkload(entry.id)}
                >
                  <span>{entry.name}</span>
                  <small>{entry.detail}</small>
                </button>
              ))}
            </div>
          </section>

          <section class="control-section control-row">
            <div>
              <div class="control-label">INTENSITY</div>
              <div class="intensity-buttons">
                {INTENSITIES.map((value) => (
                  <button
                    type="button"
                    class={intensity === value ? 'is-active' : ''}
                    disabled={running}
                    onClick={() => setIntensity(value)}
                  >
                    {value}×
                  </button>
                ))}
              </div>
            </div>
            <div class="run-buttons">
              {running ? (
                <button type="button" class="danger" onClick={stop}>Stop</button>
              ) : (
                <button type="button" class="primary" onClick={() => startRun()}>Run</button>
              )}
              <button type="button" disabled={suiteRunning || running} onClick={runSuite}>Run suite</button>
            </div>
          </section>

          <section class="reference-note">
            <strong>READ THIS WITH RENDER STATS ON</strong>
            <p><b>Page RAF</b> measures browser callbacks. <b>Fresh view</b> in the global overlay measures new textures drawn in Starfield. A gap between them isolates capture/delivery; high JS work points back to authored page code.</p>
            <p>The suite runs each workload for five seconds after a warm-up. Keep the same resolution, game scene and intensity when comparing builds.</p>
          </section>
        </aside>
      </main>

      <section class="results-panel">
        <div class="panel-title results-title">
          <span>REFERENCE RESULTS</span>
          <div>
            <button type="button" disabled={!results.length} onClick={() => setResults([])}>Clear</button>
            <button type="button" disabled={!results.length} onClick={copyResults}>{copyState}</button>
          </div>
        </div>
        {results.length ? (
          <div class="results-table" role="table" aria-label="Benchmark results">
            <div class="result-row result-head" role="row">
              <span>Workload</span><span>Load</span><span>RAF</span><span>Frame p50</span><span>Frame p95</span><span>JS p95</span><span>Timer p95</span><span>Slow</span>
            </div>
            {results.map((result, index) => (
              <div class="result-row" role="row" key={`${result.workload}-${index}`}>
                <span>{result.name}</span>
                <span>{result.intensity}×</span>
                <span>{result.fps.toFixed(1)}</span>
                <span>{result.frameP50.toFixed(2)} ms</span>
                <span class={result.frameP95 > 25 ? 'hot' : ''}>{result.frameP95.toFixed(2)} ms</span>
                <span class={result.workP95 > 8 ? 'hot' : ''}>{result.workP95.toFixed(2)} ms</span>
                <span>{result.timerP95.toFixed(2)} ms</span>
                <span>{result.slowFrames}</span>
              </div>
            ))}
          </div>
        ) : (
          <div class="results-empty">Run one workload or the full suite to establish a reference.</div>
        )}
      </section>
    </div>
  );
}

function Metric({ label, value, hot = false }: { label: string; value: string; hot?: boolean }) {
  return (
    <div class={hot ? 'metric is-hot' : 'metric'}>
      <span>{label}</span>
      <strong>{value}</strong>
    </div>
  );
}
