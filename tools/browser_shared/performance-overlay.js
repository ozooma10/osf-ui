// Injected into local top-level views only, and only in Developer mode.
// Browser timing plus optional native capture/publication telemetry; never game FPS.
(() => {
  if (window.top !== window) return;
  const bridge = window.osfui = window.osfui || {};
  if (bridge.performance) return;

  const windowMs = 5000;
  const capacity = 2048;
  const times = new Float64Array(capacity);
  const durations = new Float64Array(capacity);
  const scrolling = new Uint8Array(capacity);
  let head = 0, count = 0, previous = null, lastDraw = -Infinity;
  let enabled = false, viewVisible = true, running = false, raf = 0;
  let scrollUntil = -Infinity, observer = null, longTasksSupported = false;
  let panel = null, readout = null, graph = null;
  let total;
  let session = '', sequence = 0, sessionStart = 0, native = null, nativeAt = -Infinity;
  let hitches = [], hitchCount = 0;
  let lastScroll = -Infinity;
  const nativeRequest = action => bridge.nativePerformance?.(action, session, performance.now() <= scrollUntil,
    Number.isFinite(lastScroll) ? performance.now() - lastScroll : 60000);
  function hitch(stage, duration, at = performance.now()) {
    ++hitchCount;
    if (hitches.length === 32) hitches.shift();
    hitches.push({ stage, atMs: at - sessionStart, durationMs: duration, scrolling: at <= scrollUntil });
  }

  function clearWindow() {
    head = count = 0;
    previous = null;
    lastDraw = scrollUntil = -Infinity;
    lastScroll = -Infinity;
  }

  function reset() {
    clearWindow();
    session = `${Date.now().toString(36)}-${++sequence}`;
    sessionStart = performance.now();
    native = null; nativeAt = -Infinity;
    hitches = []; hitchCount = 0;
    total = { frames: 0, measuredMs: 0, worstMs: 0, over33ms: 0, over50ms: 0,
      longTasks: 0, longTaskMs: 0, longestTaskMs: 0 };
    // Discard observer records from before this reset.
    observer?.takeRecords();
    if (running) nativeRequest('start');
  }

  function recent(now) {
    const samples = [];
    for (let i = 0; i < count; ++i) {
      const at = (head - count + i + capacity) % capacity;
      if (times[at] > now - windowMs) {
        samples.push({ time: times[at], ms: durations[at], scroll: scrolling[at] });
      }
    }
    return samples;
  }

  function summarize(samples) {
    const sorted = samples.map(s => s.ms).sort((a, b) => a - b);
    const elapsed = sorted.reduce((a, b) => a + b, 0);
    return { frames: sorted.length, fps: elapsed ? sorted.length * 1000 / elapsed : null,
      averageMs: sorted.length ? elapsed / sorted.length : null,
      p95Ms: sorted.length ? sorted[Math.ceil(sorted.length * 0.95) - 1] : null,
      worstMs: sorted.length ? sorted[sorted.length - 1] : null,
      over33ms: sorted.filter(ms => ms > 1000 / 30 + 0.01).length,
      over50ms: sorted.filter(ms => ms > 50).length };
  }

  function snapshot() {
    const samples = recent(performance.now());
    return { source: 'browser requestAnimationFrame (not game/GPU FPS)',
      page: location.pathname, enabled, sampling: running, windowMs,
      recent: summarize(samples), scroll: summarize(samples.filter(s => s.scroll)),
      session: { ...total, longTasksSupported }, diagnosticSession: session,
      native: native ? { ...native, status: performance.now() - nativeAt > 1500 ? 'stale' : 'current',
        ageMs: performance.now() - nativeAt } : null,
      hitches: hitches.map(h => ({ ...h })), hitchesOmitted: hitchCount - hitches.length };
  }

  function mount() {
    if (panel || !document.documentElement) return;
    panel = document.createElement('div');
    panel.id = 'osfui-performance';
    // The page's fonts, theme, layout and pointer handling must not affect the HUD.
    panel.style.cssText = 'all:initial!important;position:fixed!important;top:12px!important;right:12px!important;' +
      'z-index:2147483647!important;pointer-events:none!important;contain:content!important;' +
      'width:390px!important;max-width:calc(100vw - 24px)!important;';
    const shadow = panel.attachShadow({ mode: 'closed' });
    const style = document.createElement('style');
    style.textContent = ':host{color-scheme:dark}section{box-sizing:border-box;padding:12px;background:#101923f2;' +
      'border:1px solid #698194;border-radius:5px;color:#edf7ff;font:12px/1.55 Consolas,monospace;' +
      'font-variant-numeric:tabular-nums}b{color:#8fe3cb;font-size:13px}pre{margin:6px 0;white-space:pre-wrap;' +
      'font:inherit}canvas{display:block;width:100%;height:64px}small{color:#b0c4d3;font:10px/1.4 Consolas,monospace}';
    const section = document.createElement('section');
    section.setAttribute('aria-label', 'OSF UI performance');
    const title = document.createElement('b');
    title.textContent = 'OSF UI / PERFORMANCE';
    readout = document.createElement('pre');
    const canvas = document.createElement('canvas');
    canvas.width = 316;
    canvas.height = 64;
    graph = canvas.getContext('2d');
    const note = document.createElement('small');
    note.textContent = 'Last 5s · native totals cover all views, not game FPS. Idle capture gaps are normal. Scroll hitches: >50ms. F11: hide + save logs.';
    section.append(title, readout, canvas, note);
    shadow.append(style, section);
    document.documentElement.append(panel);
  }

  function draw(now) {
    mount();
    if (!readout) return;
    const s = snapshot();
    const ms = value => Number.isFinite(value) ? value.toFixed(1) : '—';
    const n = s.native;
    const nativeText = n?.status === 'current' ?
      `Capture ${ms(n.capture.fps)} FPS / Published ${ms(n.publication.fps)} FPS\n` +
      `Capture gap: p95 ${ms(n.capture.p95GapMs)} / max ${ms(n.capture.worstGapMs)} ms\n` +
      `Publish gap: p95 ${ms(n.publication.p95GapMs)} / max ${ms(n.publication.worstGapMs)} ms\n` +
      `Queue deferrals ${n.queueDeferrals} / Scroll hitches ${n.scrollHitches}\n` :
      `Native capture: ${n ? 'stale' : bridge.nativePerformance ? 'waiting' : 'unavailable on this backend'}\n`;
    readout.textContent = `Browser ${ms(s.recent.fps)} FPS / Scroll ${ms(s.scroll.fps)}\n` + nativeText +
      `Frame ms: avg ${ms(s.recent.averageMs)} / p95 ${ms(s.recent.p95Ms)}\n` +
      `Worst: ${ms(s.recent.worstMs)} ms / session ${ms(total.worstMs)}\n` +
      `Last 5s: >33ms ${s.recent.over33ms} / >50ms ${s.recent.over50ms}\n` +
      (longTasksSupported ? `JS tasks ≥50ms: ${total.longTasks} / ${total.longTaskMs.toFixed(0)} ms total` :
        'JS long tasks: unavailable');
    if (!graph) return;
    graph.clearRect(0, 0, 316, 64);
    graph.strokeStyle = '#425668';
    for (const budget of [1000 / 60, 1000 / 30]) {
      const y = 64 - budget * 0.64;
      graph.beginPath(); graph.moveTo(0, y); graph.lineTo(316, y); graph.stroke();
    }
    for (const sample of recent(now)) {
      const x = 316 * (1 - (now - sample.time) / windowMs);
      graph.strokeStyle = sample.ms > 50 ? '#ff887f' : sample.ms > 1000 / 30 + 0.01 ? '#ffd17b' : '#8fe3cb';
      graph.beginPath(); graph.moveTo(x, 64);
      graph.lineTo(x, 64 - Math.min(100, sample.ms) * 0.64); graph.stroke();
    }
  }

  function frame(now) {
    raf = 0;
    if (!running) return;
    if (previous !== null && now > previous) {
      const delta = now - previous;
      times[head] = now;
      durations[head] = delta;
      scrolling[head] = previous <= scrollUntil ? 1 : 0;
      head = (head + 1) % capacity;
      count = Math.min(capacity, count + 1);
      ++total.frames;
      total.measuredMs += delta;
      total.worstMs = Math.max(total.worstMs, delta);
      if (delta > 1000 / 30 + 0.01) ++total.over33ms;
      if (delta > 50) { ++total.over50ms; hitch('browser-frame', delta, now); }
    }
    previous = now;
    if (now - lastDraw >= 250) { lastDraw = now; nativeRequest('sample'); draw(now); }
    raf = requestAnimationFrame(frame);
  }

  function onScroll() {
    const starting = performance.now() > scrollUntil;
    lastScroll = performance.now();
    scrollUntil = lastScroll + 200;
    if (starting) nativeRequest('sample');
  }

  function reconcile() {
    const wanted = enabled && viewVisible && !document.hidden;
    if (wanted === running) return;
    running = wanted;
    clearWindow(); // Hiding, caching or alt-tabbing is not a slow frame.
    if (running) {
      native = null; nativeAt = -Infinity;
      nativeRequest('start');
      mount();
      document.addEventListener('scroll', onScroll, { capture: true, passive: true });
      try {
        if (typeof PerformanceObserver !== 'undefined' && PerformanceObserver.supportedEntryTypes?.includes('longtask')) {
          observer = new PerformanceObserver(list => {
            if (!running) return;
            for (const task of list.getEntries()) {
              ++total.longTasks;
              total.longTaskMs += task.duration;
              total.longestTaskMs = Math.max(total.longestTaskMs, task.duration);
              hitch('js-long-task', task.duration);
            }
          });
          observer.observe({ type: 'longtask' });
          longTasksSupported = true;
        }
      } catch (_) { observer?.disconnect(); observer = null; longTasksSupported = false; }
      raf = requestAnimationFrame(frame);
    } else {
      nativeRequest(enabled ? 'pause' : 'stop');
      cancelAnimationFrame(raf);
      raf = 0;
      observer?.disconnect();
      observer = null;
      document.removeEventListener('scroll', onScroll, true);
      panel?.remove();
      panel = readout = graph = null;
    }
  }

  function show() {
    if (enabled) return;
    enabled = true;
    reset();
    reconcile();
  }

  function hide() {
    if (!enabled) return;
    console.info('[OSF UI performance] ' + JSON.stringify(snapshot()));
    enabled = false;
    if (!running) nativeRequest('stop');
    reconcile();
  }

  bridge.performance = Object.freeze({ show, hide, reset, snapshot,
    updateNative: (id, value) => {
      if (!running || id !== session || !value || typeof value !== 'object' ||
          !value.capture || !value.publication) return;
      native = value; nativeAt = performance.now();
    },
    toggle: () => enabled ? hide() : show(),
    // Transport-level visibility works with both the modern and bundled v1 SDK.
    setVisible: visible => { viewVisible = visible; reconcile(); } });
  document.addEventListener('visibilitychange', reconcile);
  reset();
})();
