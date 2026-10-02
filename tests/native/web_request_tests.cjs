const { test } = require('node:test');
const assert = require('node:assert/strict');
const fs = require('node:fs');
const vm = require('node:vm');

const source = fs.readFileSync('web/osfui.js', 'utf8');

function bridge() {
  const maps = [];
  const timers = new Map();
  const sent = [];
  let nextTimer = 0;
  // Observe retained entries without exposing private state in the shipped helper.
  class TrackedMap extends Map {
    constructor() { super(); maps.push(this); }
  }
  const window = { osfui: { postMessage: text => sent.push(JSON.parse(text)) } };
  vm.runInNewContext(source, {
    window, console, Map: TrackedMap,
    setTimeout: fn => { const id = ++nextTimer; timers.set(id, fn); return id; },
    clearTimeout: id => timers.delete(id),
  });
  return {
    api: window.osfui, sent, timers,
    retained: () => maps.reduce((count, map) => count + map.size, 0),
  };
}

for (const timeoutMs of [10000, 0]) {
  test(`serialization failure releases request resources (timeout ${timeoutMs})`, async () => {
    const { api, sent, timers, retained } = bridge();
    const payload = {};
    payload.self = payload;
    await assert.rejects(api.request('example.read', payload, { timeoutMs }), { name: 'TypeError' });
    assert.equal(sent.length, 1); // Only the initial hello was posted.
    assert.equal(timers.size, 0);
    assert.equal(retained(), 0);
  });

  test(`transport failure releases request resources (timeout ${timeoutMs})`, async () => {
    const { api, timers, retained } = bridge();
    const failure = new Error('transport unavailable');
    api.postMessage = () => { throw failure; };
    await assert.rejects(api.request('example.read', {}, { timeoutMs }), error => error === failure);
    assert.equal(timers.size, 0);
    assert.equal(retained(), 0);
  });
}

test('successful requests retain resources until their reply arrives', async () => {
  const { api, sent, timers, retained } = bridge();
  const request = api.request('example.read', {});
  assert.equal(timers.size, 1);
  assert.equal(retained(), 1);
  api.onMessage(JSON.stringify({ kind: 'reply', id: sent.at(-1).id, payload: 42 }));
  assert.equal(await request, 42);
  assert.equal(timers.size, 0);
  assert.equal(retained(), 0);
});

test('a transport may deliver a reply synchronously during posting', async () => {
  const { api, timers, retained } = bridge();
  api.postMessage = text => {
    const request = JSON.parse(text);
    api.onMessage(JSON.stringify({ kind: 'reply', id: request.id, payload: 42 }));
  };
  assert.equal(await api.request('example.read', {}), 42);
  assert.equal(timers.size, 0);
  assert.equal(retained(), 0);
});
