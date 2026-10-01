// No browser/game dependency: exercise the shipped facade against v2 envelopes.
const { test } = require('node:test');
const assert = require('node:assert/strict');
const fs = require('node:fs');
const vm = require('node:vm');
const source = fs.readFileSync('src/Compat/V1/web/osfui.js', 'utf8');
test('legacy ready, sends, typed replies, errors and gamepad events use v2 transport', async () => {
  const sent = [];
  const window = { osfui: { postMessage: text => sent.push(JSON.parse(text)) } };
  vm.runInNewContext(source, { window, console, setTimeout, clearTimeout });
  const api = window.osfui;
  const receive = message => api.onMessage(JSON.stringify(message));
  assert.equal(sent[0].name, 'osfui.hello');
  assert.equal(sent[0].kind, 'send');
  receive({ kind: 'ready', payload: { version: '2.0.0' } });
  assert.equal((await api.ready).version, '2.0.0');
  api.emit('starcade.arcade.state.get');
  assert.equal(sent.at(-1).payload.command, 'starcade.arcade.state.get');
  let received;
  const off = api.on('ui.gamepad', (payload, envelope) => received = [payload, envelope]);
  receive({ kind: 'event', name: 'ui.gamepad', payload: { kind: 'button' } });
  assert.equal(received[0].kind, 'button');
  assert.equal(received[1].type, 'ui.gamepad');
  off();
  received = undefined;
  receive({ kind: 'event', name: 'ui.gamepad', payload: {} });
  assert.equal(received, undefined);
  const reply = api.call('GetPluginsRequest', {});
  const request = sent.at(-1);
  assert.equal(request.kind, 'request');
  receive({ kind: 'reply', id: request.id, payload: { __osfuiV1Reply: true, type: 'plugins', payload: { count: 3 } } });
  assert.equal((await reply).count, 3);
  const envelope = api.request('GetFormsRequest');
  receive({ kind: 'reply', id: sent.at(-1).id, payload: { __osfuiV1Reply: true, type: 'forms', payload: [] } });
  assert.equal((await envelope).type, 'forms');
  const failure = api.call('GetFormsRequest');
  receive({ kind: 'error', id: sent.at(-1).id, payload: { code: 'rejected', message: 'unavailable' } });
  await assert.rejects(failure, error => error.code === 'rejected');
});

test('retained typed legacy state replays once, unsubscribes, and resets only script-owned keys', () => {
  const window = { osfui: { postMessage() {} } };
  vm.runInNewContext(source, { window, console, setTimeout, clearTimeout });
  const api = window.osfui;
  const receive = message => api.onMessage(JSON.stringify(message));
  receive({ kind: 'state', mod: 'x2357.ssse', key: 'stableIds', value: [3, 18] });
  receive({ kind: 'state', mod: 'x2357.ssse', key: 'native', value: true });
  const seen = [];
  const off = api.data.on('STABLEIDS', (value, payload, message) => seen.push([value, payload, message]));
  assert.equal(seen.length, 1);
  assert.deepEqual(Array.from(seen[0][0]), [3, 18]);
  assert.equal(seen[0][2].type, 'data.state');
  receive({ kind: 'event', name: 'data.reset', payload: { keys: ['stableids'] } });
  assert.equal(seen.length, 2);
  assert.equal(seen[1][0], null);
  assert.equal(api.data.get('stableIds'), undefined);
  assert.equal(api.data.get('native'), true);
  off();
  receive({ kind: 'state', mod: 'x2357.ssse', key: 'stableIds', value: [8] });
  assert.equal(seen.length, 2);
  api.data.on('stableIds', value => assert.equal(value[0], 8));
});

test('legacy chart timeout is carried to the adapter; errors, late replies and malformed input', async () => {
  const sent = [], timers = new Map(); let nextTimer = 1;
  const window = { osfui: { postMessage: text => sent.push(JSON.parse(text)) } };
  vm.runInNewContext(source, { window, console,
    setTimeout: (fn, ms) => { const id = nextTimer++; timers.set(id, { fn, ms }); return id; },
    clearTimeout: id => timers.delete(id) });
  const api = window.osfui;
  api.onMessage('{bad'); api.onMessage('null');
  api.action('buy', 3, 25);
  assert.deepEqual(sent.at(-1).payload, { command: 'ui.action', action: 'buy', args: [3, 25] });
  const chart = api.call('ui.papyrusRequest', { request: 'candles', args: [3, 30] }, { timeoutMs: 60000 });
  assert.equal(sent.at(-1).payload.__osfuiV1TimeoutMs, 60000);
  const id = sent.at(-1).id;
  const rejected = assert.rejects(chart, error => error.code === 'timeout');
  timers.values().next().value.fn();
  await rejected;
  api.onMessage(JSON.stringify({ kind: 'reply', id, payload: { __osfuiV1Reply: true, type: 'papyrus.result', payload: { value: [99] } } }));
  const request = api.papyrus.request('intraday', 3);
  assert.equal(sent.at(-1).payload.__osfuiV1TimeoutMs, 15000);
  api.onMessage(JSON.stringify({ kind: 'error', id: sent.at(-1).id, payload: { code: 'papyrus-timeout' } }));
  await assert.rejects(request, error => error.code === 'papyrus-timeout');
});
