// Deterministic frame timings and real transport scripts; no browser or game needed.
const { test } = require('node:test');
const assert = require('node:assert/strict');
const fs = require('node:fs');
const vm = require('node:vm');
const script = fs.readFileSync('tools/browser_shared/performance-overlay.js', 'utf8');

function fixture(backend = 'cef', { child = false, longTasks = true } = {}) {
  let now = 0, next = 0, receive, activeObserver;
  const frames = new Map(), listeners = new Map(), logs = [], panels = [], posted = [];
  const element = () => ({ style: {}, textContent: '', append() {}, setAttribute() {},
    attachShadow: () => element(), getContext: () => null,
    remove() { panels.splice(panels.indexOf(this), 1); } });
  const document = { hidden: false, activeElement: { blur() {} },
    documentElement: { append: el => panels.push(el) }, createElement: element,
    addEventListener: (name, fn) => listeners.set(name, fn),
    removeEventListener: name => listeners.delete(name) };
  const window = { __osfCefPost() {} };
  window.top = child ? {} : window;
  const chrome = { webview: { postMessage: value => posted.push(value), addEventListener: (_, fn) => {
    receive = message => fn({ data: JSON.stringify(message) });
  } } };
  window.chrome = chrome;
  class Observer {
    static supportedEntryTypes = longTasks ? ['longtask'] : [];
    constructor(fn) { this.fn = fn; activeObserver = this; }
    observe() { this.active = true; }
    disconnect() { this.active = false; }
    takeRecords() { return []; }
  }
  const context = vm.createContext({ window, document, chrome, location: { pathname: '/acme/menu/index.html' },
    console: { info: line => logs.push(line) }, performance: { now: () => now }, PerformanceObserver: Observer,
    requestAnimationFrame: fn => { frames.set(++next, fn); return next; },
    cancelAnimationFrame: id => frames.delete(id) });
  vm.runInContext(fs.readFileSync(backend === 'cef' ? 'tools/cef_host/bridge.js' :
    'tools/webview2_host/scripts/bridge-shim.js', 'utf8'), context);
  if (backend === 'cef') receive = message => window.__osfCefReceive(JSON.stringify(message));
  vm.runInContext(script, context);
  return { api: window.osfui.performance, frames, document, listeners, logs, panels, context, posted,
    receive, toggle: () => receive({ kind: 'event', name: 'osfui.debug.performance', payload: {} }),
    visibility: visible => receive({ kind: 'event', name: 'ui.visibility', payload: { visible } }),
    step(delta) { now += delta; const callbacks = [...frames.values()]; frames.clear(); callbacks.forEach(fn => fn(now)); },
    task(duration) { if (activeObserver?.active) activeObserver.fn({ getEntries: () => [{ duration }] }); },
    observing: () => !!activeObserver?.active };
}

for (const backend of ['cef', 'webview2']) {
  test(`${backend}: opt-in, repeat-safe injection, real transport toggle and teardown`, () => {
    const f = fixture(backend);
    assert.equal(f.frames.size, 0);
    assert.equal(f.panels.length, 0);
    assert.equal(f.observing(), false);
    f.toggle();
    vm.runInContext(script, f.context);
    assert.equal(f.frames.size, 1);
    assert.equal(f.panels.length, 1);
    assert.equal(f.observing(), true);
    f.step(0); f.step(16);
    f.toggle();
    assert.equal(f.frames.size, 0);
    assert.equal(f.panels.length, 0);
    assert.equal(f.observing(), false);
    assert.equal(f.listeners.has('scroll'), false);
    assert.equal(f.logs.length, 1);
    const report = JSON.parse(f.logs[0].slice('[OSF UI performance] '.length));
    assert.equal(report.session.frames, 1);
    assert.equal(report.recent.fps, 62.5);
    f.toggle();
    assert.equal(f.api.snapshot().session.frames, 0);
  });

  test(`${backend}: cached views and document hiding do not create false multi-second hitches`, () => {
    const f = fixture(backend);
    f.toggle(); f.step(0); f.step(16);
    f.visibility(false);
    assert.equal(f.frames.size, 0);
    assert.equal(f.observing(), false);
    f.step(10000);
    f.visibility(true);
    f.step(0); f.step(16);
    assert.equal(f.api.snapshot().session.worstMs, 16);
    assert.equal(f.api.snapshot().recent.frames, 1);
    f.document.hidden = true;
    f.listeners.get('visibilitychange')();
    f.step(30000);
    f.document.hidden = false;
    f.listeners.get('visibilitychange')();
    f.step(0); f.step(16);
    assert.equal(f.api.snapshot().session.worstMs, 16);
    f.visibility(false); f.api.hide(); f.visibility(true);
    assert.equal(f.frames.size, 0);
  });
}

test('frame rates, nearest-rank p95, thresholds, scrolling and retained hitch measurements', () => {
  const f = fixture();
  f.toggle(); f.step(0);
  f.step(10); f.step(20);
  f.listeners.get('scroll')();
  f.step(40); f.step(100);
  f.task(80); f.task(55);
  const s = f.api.snapshot();
  assert.equal(s.recent.fps, 4000 / 170);
  assert.equal(s.recent.averageMs, 42.5);
  assert.equal(s.recent.p95Ms, 100);
  assert.equal(s.recent.over33ms, 2);
  assert.equal(s.recent.over50ms, 1);
  assert.equal(s.scroll.frames, 2);
  assert.equal(s.scroll.fps, 2000 / 140);
  assert.equal(s.session.longTasks, 2);
  assert.equal(s.session.longTaskMs, 135);
  assert.equal(s.session.longestTaskMs, 80);
  for (let i = 0; i < 600; ++i) f.step(10);
  assert.equal(f.api.snapshot().recent.worstMs, 10);
  assert.equal(f.api.snapshot().session.worstMs, 100);
  assert.equal(f.api.snapshot().scroll.fps, null);
  f.api.reset();
  f.step(10);
  assert.equal(f.api.snapshot().session.frames, 0); // new timing baseline
  assert.equal(f.api.snapshot().session.longTasks, 0);
});

test('healthy 30/60 Hz frames, unavailable long-task API and ring-buffer wrap', () => {
  const f = fixture('cef', { longTasks: false });
  f.toggle(); f.step(0);
  for (let i = 0; i < 2200; ++i) f.step(1000 / 60);
  const s = f.api.snapshot();
  assert.ok(Math.abs(s.recent.fps - 60) < 0.001);
  assert.equal(s.session.frames, 2200);
  assert.equal(s.session.over33ms, 0);
  assert.equal(s.session.longTasksSupported, false);
  f.step(1000 / 30);
  assert.equal(f.api.snapshot().session.over33ms, 0);
});

test('child frames never create a duplicate overlay or sampler', () => {
  const f = fixture('webview2', { child: true });
  assert.equal(f.api, undefined);
  f.toggle();
  assert.equal(f.frames.size, 0);
  assert.equal(f.panels.length, 0);
});

test('native telemetry uses session IDs, expires, logs with F11 and stops when hidden', () => {
  const f = fixture('webview2');
  const messages = () => f.posted.filter(v => v.startsWith('__osfuiPerformance:'))
    .map(v => JSON.parse(v.slice('__osfuiPerformance:'.length)));
  assert.equal(messages().length, 0);
  f.toggle();
  const id = f.api.snapshot().diagnosticSession;
  assert.equal(messages()[0].action, 'start');
  const payload = { capture: { fps: 13 }, publication: { fps: 12 }, queueDeferrals: 1,
    scrollHitches: 2, hitches: [{ stage: 'capture', gapMs: 80 }] };
  f.receive({ kind: 'osfui.nativePerformance', session: 'old', payload });
  assert.equal(f.api.snapshot().native, null);
  f.receive({ kind: 'osfui.nativePerformance', session: id, payload });
  assert.equal(f.api.snapshot().native.capture.fps, 13);
  assert.equal(f.api.snapshot().native.status, 'current');
  f.step(1600);
  assert.equal(f.api.snapshot().native.status, 'stale');
  f.listeners.get('scroll')();
  assert.equal(messages().at(-1).scrolling, true);
  f.toggle();
  assert.equal(messages().at(-1).action, 'stop');
  const logged = JSON.parse(f.logs[0].slice('[OSF UI performance] '.length));
  assert.equal(logged.diagnosticSession, id);
  assert.equal(logged.native.capture.fps, 13);
  f.toggle();
  const next = f.api.snapshot().diagnosticSession;
  assert.notEqual(next, id);
  f.receive({ kind: 'osfui.nativePerformance', session: id, payload });
  assert.equal(f.api.snapshot().native, null);
  f.visibility(false);
  assert.equal(messages().at(-1).action, 'pause');
  f.step(5000);
  f.visibility(true);
  assert.equal(messages().at(-1).action, 'start');
  assert.equal(messages().at(-1).session, next);
  f.api.reset();
  assert.notEqual(f.api.snapshot().diagnosticSession, next);
  assert.equal(messages().at(-1).action, 'start');
});

test('browser hitch log is bounded, survives rolling window expiry and resets explicitly', () => {
  const f = fixture('webview2');
  f.toggle(); f.step(0);
  for (let i = 0; i < 100; ++i) f.step(70);
  let s = f.api.snapshot();
  assert.equal(s.hitches.length, 32);
  assert.equal(s.hitchesOmitted, 68);
  assert.equal(s.hitches.at(-1).durationMs, 70);
  assert.equal(s.hitches.at(-1).atMs, 7000);
  for (let i = 0; i < 600; ++i) f.step(10);
  s = f.api.snapshot();
  assert.equal(s.recent.over50ms, 0);
  assert.equal(s.hitches.length, 32);
  f.task(80);
  assert.equal(f.api.snapshot().hitches.at(-1).stage, 'js-long-task');
  f.api.reset();
  assert.equal(f.api.snapshot().hitches.length, 0);
  assert.equal(f.api.snapshot().hitchesOmitted, 0);
});
