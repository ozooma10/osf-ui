const { test } = require('node:test');
const assert = require('node:assert/strict');
const fs = require('node:fs');
const vm = require('node:vm');
const crypto = require('node:crypto');

const source = fs.readFileSync('tools/webview2_host/audio-extension/collector.js', 'utf8');
const sourceUrl = 'https://example.test/movie';

function fixture(options = {}) {
  const sent = [], contexts = [], intervals = new Map();
  const time = { wall: 100000, audio: 1 };
  let timerId = 0, captureRequests = 0;
  const track = { stopped: 0, handlers: {}, stop() { this.stopped++; },
    addEventListener(name, handler) { this.handlers[name] = handler; } };
  const stream = { getAudioTracks: () => [track], getTracks: () => [track] };
  class Param {
    constructor(value = 0) { this.value = value; this.events = []; }
    setValueAtTime(value, at) { this.value = at <= time.audio ? value : this.value; this.events.push({ value, at }); }
    cancelScheduledValues(at) { this.events = this.events.filter(event => event.at < at); }
  }
  class Node {
    constructor() { this.connections = new Set(); }
    connect(node) { this.connections.add(node); }
    disconnect(node) { if (node) this.connections.delete(node); else this.connections.clear(); }
  }
  class Context {
    constructor() {
      this.state = 'suspended'; this.sampleRate = 48000; this.listener = {}; this.handlers = {};
      this.gains = []; this.panners = []; this.destination = new Node();
      for (const prefix of ['position', 'forward', 'up']) for (const axis of 'XYZ') this.listener[prefix + axis] = new Param();
      contexts.push(this);
    }
    get currentTime() { return time.audio; }
    createMediaStreamSource() { return new Node(); }
    createGain() { const node = new Node(); node.gain = new Param(1); this.gains.push(node); return node; }
    createPanner() {
      const node = new Node(); for (const axis of 'XYZ') node['position' + axis] = new Param();
      this.panners.push(node); return node;
    }
    addEventListener(name, handler) { this.handlers[name] = handler; }
    async resume() { this.state = 'running'; }
    async close() { this.state = 'closed'; }
  }
  const chrome = {
    runtime: {},
    webview: { postMessage: message => sent.push(JSON.parse(message)) },
    tabs: { query: (_query, callback) => callback(options.tabs ?? [{ id: 7, url: sourceUrl }]) },
    tabCapture: { getMediaStreamId: (request, callback) => {
      captureRequests++;
      assert.equal(request.targetTabId, 7);
      if (options.denied) chrome.runtime.lastError = { message: 'Not invoked' };
      callback(options.denied ? undefined : 'private-stream-id');
      delete chrome.runtime.lastError;
    } },
  };
  const window = { addEventListener() {} };
  vm.runInNewContext(source, { window, chrome, URL, Date: { now: () => time.wall },
    AudioContext: Context, navigator: { mediaDevices: { getUserMedia: () => options.mediaPromise ?? Promise.resolve(stream) } },
    setInterval: callback => { const id = ++timerId; intervals.set(id, callback); return id; },
    clearInterval: id => intervals.delete(id) });
  const scene = (overrides = {}) => ({ position: [0, 0, 0], front: [0, 1, 0], up: [0, 0, 1],
    emitters: [{ id: 1, position: [0, 1, 0] }], gain: 1, muted: false, deadlineUtcMs: time.wall + 1000, ...overrides });
  return { api: window.osfAudio, sent, contexts, intervals, time, track, stream, scene,
    captureRequests: () => captureRequests, start: () => window.osfAudio.configure({ sourceUrl }) };
}

test('production extension keeps the fixed ID and narrow permissions', () => {
  const manifest = JSON.parse(fs.readFileSync('tools/webview2_host/audio-extension/manifest.json', 'utf8'));
  const bytes = crypto.createHash('sha256').update(Buffer.from(manifest.key, 'base64')).digest().subarray(0, 16);
  assert.equal([...bytes].map(byte => String.fromCharCode(97 + (byte >> 4), 97 + (byte & 15))).join(''),
    'nbligbkobhajkimckkfnadnghkimjcld');
  assert.deepEqual(manifest.permissions, ['tabCapture', 'tabs']);
  for (const property of ['host_permissions', 'content_scripts', 'externally_connectable', 'background'])
    assert.equal(property in manifest, false);
});

test('capture starts silent and cannot create a second stream', async () => {
  const f = fixture();
  assert.equal(f.sent[0].state, 'ready');
  assert.equal(await f.start(), true);
  assert.equal(f.contexts[0].gains[1].gain.value, 0);
  assert.equal(await f.start(), false);
  assert.equal(f.captureRequests(), 1);
});

test('ambiguous source selection and capture denial fail without media output', async () => {
  for (const options of [{ tabs: [] }, { tabs: [{ id: 7, url: sourceUrl }, { id: 8, url: sourceUrl }] }, { denied: true }]) {
    const f = fixture(options);
    assert.equal(await f.start(), false);
    assert.equal(f.sent.at(-1).state, 'failed');
    assert.equal(f.contexts.length, 0);
  }
});

test('listener and emitter coordinates map Z-up to Web Audio and schedule exact expiry', async () => {
  const f = fixture(); await f.start();
  assert.equal(f.api.setScene(f.scene({ position: [1, 2, 3], emitters: [{ id: 1, position: [4, 5, 6] }] })), true);
  const c = f.contexts[0];
  assert.deepEqual(['X', 'Y', 'Z'].map(axis => c.listener['position' + axis].value), [1, 3, -2]);
  assert.deepEqual(['X', 'Y', 'Z'].map(axis => c.listener['forward' + axis].value), [0, 0, -1]);
  assert.deepEqual(['X', 'Y', 'Z'].map(axis => c.panners[0]['position' + axis].value), [4, 6, -5]);
  assert.equal(c.panners[0].distanceModel, 'linear');
  assert.equal(c.panners[0].refDistance, 1); assert.equal(c.panners[0].maxDistance, 20);
  assert.deepEqual(c.gains[1].gain.events.at(-1), { value: 0, at: 2 });
});

test('expired or malformed updates silence an active graph without releasing capture', async () => {
  const f = fixture(); await f.start(); f.api.setScene(f.scene());
  for (const change of [{ deadlineUtcMs: f.time.wall - 1 }, { deadlineUtcMs: f.time.wall + 1001 },
    { front: [0, 2, 0] }, { position: [NaN, 0, 0] }, { gain: 2 },
    { emitters: [{ id: 1, position: [0, 0, 0] }, { id: 1, position: [0, 0, 0] }] },
    { emitters: Array.from({ length: 33 }, (_, i) => ({ id: i + 1, position: [0, 0, 0] })) }]) {
    assert.equal(f.api.setScene(f.scene(change)), false);
    assert.equal(f.contexts[0].gains[1].gain.value, 0);
    assert.equal(f.sent.at(-1).state, 'silent');
    assert.equal(f.track.stopped, 0);
    assert.equal(f.api.setScene(f.scene()), true);
  }
});

test('scene watchdog mutes and a fresh scene recovers the existing capture', async () => {
  const f = fixture(); await f.start(); f.api.setScene(f.scene());
  f.time.wall += 1001;
  for (const tick of f.intervals.values()) tick();
  assert.equal(f.sent.at(-1).reason, 'scene-expired');
  assert.equal(f.contexts[0].gains[1].gain.value, 0);
  assert.equal(f.track.stopped, 0);
  assert.equal(f.api.setScene(f.scene()), true);
  assert.equal(f.captureRequests(), 1);
});

test('normalization caps coincident sources but distant sources do not reduce a nearby screen', async () => {
  const f = fixture(); await f.start();
  const near = { id: 1, position: [0, 1, 0] }, far = { id: 2, position: [0, 40, 0] };
  f.api.setScene(f.scene({ emitters: [near, far] }));
  assert.equal(f.api.snapshot().normalization, 1);
  f.api.setScene(f.scene({ emitters: [near, { ...near, id: 2 }] }));
  assert.equal(f.api.snapshot().normalization, 2);
  assert.equal(f.contexts[0].gains[2].gain.value, 0.5);
  assert.equal(f.contexts[0].gains[3].gain.value, 0.5);
});

test('user mute and empty emitter scenes stay silent while retaining capture', async () => {
  const f = fixture(); await f.start();
  for (const change of [{ muted: true }, { emitters: [] }, { gain: 0 }]) {
    f.api.setScene(f.scene(change));
    assert.equal(f.contexts[0].gains[1].gain.value, 0);
    assert.equal(f.api.snapshot().captured, true);
    assert.equal(f.track.stopped, 0);
  }
});

test('track failure silences and reports immediately, with teardown reserved for native stop', async () => {
  const f = fixture(); await f.start(); f.api.setScene(f.scene());
  f.track.handlers.ended();
  assert.equal(f.sent.at(-1).state, 'failed');
  assert.equal(f.contexts[0].gains[1].gain.value, 0);
  assert.equal(f.track.stopped, 0);
  assert.equal(f.api.setScene(f.scene()), false);
  assert.equal(await f.api.stop(), true);
  assert.equal(f.track.stopped, 1);
  assert.equal(f.intervals.size, 0);
  assert.equal(f.contexts[0].state, 'closed');
});

test('capture completion after native stop cannot resurrect its audio graph', async () => {
  let completeMedia;
  const mediaPromise = new Promise(resolve => { completeMedia = resolve; });
  const f = fixture({ mediaPromise });
  const pending = f.start();
  await new Promise(resolve => setImmediate(resolve));
  assert.equal(await f.api.stop(), true);
  completeMedia(f.stream);
  assert.equal(await pending, false);
  assert.equal(f.contexts.length, 0);
  assert.equal(f.track.stopped, 1);
  assert.equal(f.api.snapshot().captured, false);
});
