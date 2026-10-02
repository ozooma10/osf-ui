// No browser/game dependency: exercise the shipped facade against v2 envelopes.
const { test } = require('node:test');
const assert = require('node:assert/strict');
const fs = require('node:fs');
const vm = require('node:vm');
const source = fs.readFileSync('src/Compat/V1/web/osfui.js', 'utf8');
const shim = fs.readFileSync('tools/webview2_host/scripts/bridge-shim.js', 'utf8');
const transport = fs.readFileSync('src/Compat/V1/web-transport.js', 'utf8');

function injectedBridge(legacy = true) {
  const sent = [];
  let receive;
  const chrome = { webview: {
    postMessage: text => sent.push(JSON.parse(text)),
    addEventListener: (name, fn) => { assert.equal(name, 'message'); receive = fn; },
  } };
  const window = { chrome };
  const context = vm.createContext({ window, chrome, document: {}, console, setTimeout, clearTimeout });
  vm.runInContext(shim, context);
  if (legacy) vm.runInContext(transport, context);
  return { context, api: window.osfui, sent, receive: message => receive({ data: message }) };
}

test('bundled 1.x wire requests receive their typed close reply and original correlation ID', () => {
  const { api, sent, receive } = injectedBridge();
  const received = [];
  api.onMessage = text => received.push(JSON.parse(text));
  assert.deepEqual(sent, [{ kind: 'send', name: 'osfui.hello', payload: {} }]);
  receive({ kind: 'ready', payload: { version: '2.0.0' } });
  assert.equal(received.at(-1).type, 'runtime.ready');

  // Console Command Center's bundled helper sends this exact 1.x envelope.
  api.postMessage(JSON.stringify({ type: 'ui.command', requestId: 'q1',
    payload: { command: 'console.command-center.close' } }));
  assert.deepEqual(sent.at(-1), { kind: 'request', name: 'console.command-center.close',
    id: 'q1', payload: { command: 'console.command-center.close' } });
  receive({ kind: 'reply', id: 'q1', payload: { __osfuiV1Reply: true,
    type: 'console.command-center.closeResult', payload: { ok: true } } });
  assert.deepEqual(received.at(-1), { type: 'console.command-center.closeResult',
    requestId: 'q1', payload: { ok: true } });
  receive({ kind: 'error', id: 'q2', payload: { code: 'unavailable', message: 'close failed' } });
  assert.deepEqual(received.at(-1), { type: 'ui.error', requestId: 'q2',
    payload: { code: 'unavailable', message: 'close failed' } });

  api.postMessage(JSON.stringify({ type: 'ui.command', payload: { command: 'view.ready' } }));
  assert.equal(sent.at(-1).kind, 'send');
  assert.equal(Object.hasOwn(sent.at(-1), 'id'), false);
  // Preserve malformed IDs so native validation rejects them instead of executing a send.
  api.postMessage(JSON.stringify({ type: 'ui.command', requestId: null, payload: { command: 'close' } }));
  assert.equal(sent.at(-1).kind, 'request');
  assert.equal(sent.at(-1).id, null);

  api.onMessage = null;
  receive({ kind: 'event', name: 'ui.gamepad', payload: { button: 'back' } });
  api.onMessage = text => received.push(JSON.parse(text));
  assert.deepEqual(received.at(-1), { type: 'ui.gamepad', payload: { button: 'back' } });
  assert.equal(sent.filter(message => message.name === 'osfui.hello').length, 1);
});

test('injected legacy translation also supports the shared facade without duplicate greetings', async () => {
  const { context, api, sent, receive } = injectedBridge();
  vm.runInContext(source, context);
  assert.equal(sent.length, 1);
  receive({ kind: 'ready', payload: { version: '2.0.0' } });
  assert.equal((await api.ready).version, '2.0.0');
  const closing = api.call('console.command-center.close');
  receive({ kind: 'reply', id: sent.at(-1).id, payload: { __osfuiV1Reply: true,
    type: 'console.command-center.closeResult', payload: { ok: true } } });
  assert.equal((await closing).ok, true);
  receive({ kind: 'state', mod: 'console.command-center', key: 'status', value: 'closed' });
  assert.equal(api.data.get('status'), 'closed');
});

test('modern injected bridge retains unmodified 2.0 messages and page-initiated greeting', () => {
  const { api, sent, receive } = injectedBridge(false);
  const received = [];
  api.onMessage = text => received.push(JSON.parse(text));
  assert.equal(sent.length, 0);
  const request = { kind: 'request', name: 'close', id: 'q1', payload: {} };
  api.postMessage(JSON.stringify(request));
  assert.deepEqual(sent.at(-1), request);
  const reply = { kind: 'reply', id: 'q1', payload: { ok: true } };
  receive(reply);
  assert.deepEqual(received.at(-1), reply);
});

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
