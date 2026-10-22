// Injected into local top-level views only, and only in Developer mode.
// Measures browser animation callbacks, not GPU time or game presentation FPS.
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

  function clearWindow() {
    head = count = 0;
    previous = null;
    lastDraw = scrollUntil = -Infinity;
  }

  function reset() {
    clearWindow();
    total = { frames: 0, measuredMs: 0, worstMs: 0, over33ms: 0, over50ms: 0,
      longTasks: 0, longTaskMs: 0, longestTaskMs: 0 };
    // Discard observer records from before this reset.
    observer?.takeRecords();
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
      session: { ...total, longTasksSupported } };
  }

  function mount() {
    if (panel || !document.documentElement) return;
    panel = document.createElement('div');
    panel.id = 'osfui-performance';
    // The page's fonts, theme, layout and pointer handling must not affect the HUD.
    panel.style.cssText = 'all:initial!important;position:fixed!important;top:12px!important;right:12px!important;' +
      'z-index:2147483647!important;pointer-events:none!important;contain:content!important;' +
      'width:340px!important;max-width:calc(100vw - 24px)!important;';
    const shadow = panel.attachShadow({ mode: 'closed' });
    const style = document.createElement('style');
    style.textContent = ':host{color-scheme:dark}section{box-sizing:border-box;padding:12px;background:#101923f2;' +
      'border:1px solid #698194;border-radius:5px;color:#edf7ff;font:12px/1.55 Consolas,monospace;' +
      'font-variant-numeric:tabular-nums}b{color:#8fe3cb;font-size:13px}pre{margin:6px 0;white-space:pre-wrap;' +
      'font:inherit}canvas{display:block;width:100%;height:64px}small{color:#b0c4d3;font:10px/1.4 Consolas,monospace}';
    const section = document.createElement('section');
    section.setAttribute('aria-label', 'OSF UI browser performance');
    const title = document.createElement('b');
    title.textContent = 'OSF UI / BROWSER PERFORMANCE';
    readout = document.createElement('pre');
    const canvas = document.createElement('canvas');
    canvas.width = 316;
    canvas.height = 64;
    graph = canvas.getContext('2d');
    const note = document.createElement('small');
    note.textContent = 'Last 5s · graph 0–100ms · lines 16.7 / 33.3ms. F11: hide + log summary. Browser timing only.';
    section.append(title, readout, canvas, note);
    shadow.append(style, section);
    document.documentElement.append(panel);
  }

  function draw(now) {
    mount();
    if (!readout) return;
    const s = snapshot();
    const ms = value => value === null ? '—' : value.toFixed(1);
    readout.textContent = `FPS ${ms(s.recent.fps)}   Scroll ${ms(s.scroll.fps)}\n` +
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
      if (delta > 50) ++total.over50ms;
    }
    previous = now;
    if (now - lastDraw >= 250) { lastDraw = now; draw(now); }
    raf = requestAnimationFrame(frame);
  }

  function onScroll() { scrollUntil = performance.now() + 200; }

  function reconcile() {
    const wanted = enabled && viewVisible && !document.hidden;
    if (wanted === running) return;
    running = wanted;
    clearWindow(); // Hiding, caching or alt-tabbing is not a slow frame.
    if (running) {
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
            }
          });
          observer.observe({ type: 'longtask' });
          longTasksSupported = true;
        }
      } catch (_) { observer?.disconnect(); observer = null; longTasksSupported = false; }
      raf = requestAnimationFrame(frame);
    } else {
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
    reconcile();
  }

  bridge.performance = Object.freeze({ show, hide, reset, snapshot,
    toggle: () => enabled ? hide() : show(),
    // Transport-level visibility works with both the modern and bundled v1 SDK.
    setVisible: visible => { viewVisible = visible; reconcile(); } });
  document.addEventListener('visibilitychange', reconcile);
  reset();
})();
