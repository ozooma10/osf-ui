// Exercise the real injected scripts and SDK together without starting a browser.
const { test } = require('node:test');
const assert = require('node:assert/strict');
const fs = require('node:fs');
const vm = require('node:vm');
const bridge = fs.readFileSync('tools/cef_host/bridge.js', 'utf8');
const modern = fs.readFileSync('web/osfui.js', 'utf8');
const legacy = fs.readFileSync('src/Compat/V1/web/osfui.js', 'utf8');
const adapter = fs.readFileSync('src/Compat/V1/web-transport.js', 'utf8');

function fixture() {
  const sent = [], blurred = [];
  const window = { __osfCefPost: text => sent.push(JSON.parse(text)) };
  const context = vm.createContext({ window, console, setTimeout, clearTimeout,
    document: { activeElement: { blur: () => blurred.push(true) } } });
  vm.runInContext(bridge, context);
  return { window, context, sent, blurred, api: window.osfui,
    receive: message => window.__osfCefReceive(JSON.stringify(message)) };
}

test('CEF bridge preserves buffered startup, modern requests, state and gamepad events', async () => {
  const { api, context, sent, receive } = fixture();
  receive({ kind: 'ready', payload: { mod: 'acme' } });
  receive({ kind: 'state', mod: 'acme', key: 'count', value: 4 });
  vm.runInContext(modern, context);
  assert.equal(sent[0].name, 'osfui.hello');
  assert.equal(api.state.get('count'), 4);
  const reply = api.request('change', { value: 5 });
  assert.deepEqual(sent.at(-1), { kind: 'request', name: 'change', id: 'q1', payload: { value: 5 } });
  receive({ kind: 'reply', id: 'q1', payload: { ok: true } });
  assert.equal((await reply).ok, true);
  let button;
  api.on('ui.gamepad', event => { button = event.button; });
  receive({ kind: 'event', name: 'ui.gamepad', payload: { button: 'confirm' } });
  assert.equal(button, 'confirm');
});

test('CEF injection supports the existing legacy facade and correlation IDs', async () => {
  const { api, context, sent, receive } = fixture();
  vm.runInContext(adapter, context);
  vm.runInContext(legacy, context);
  assert.equal(sent.filter(message => message.name === 'osfui.hello').length, 1);
  receive({ kind: 'ready', payload: { version: '2.0.0' } });
  assert.equal((await api.ready).version, '2.0.0');
  const reply = api.call('menu.close');
  receive({ kind: 'reply', id: sent.at(-1).id, payload: {
    __osfuiV1Reply: true, type: 'menu.closed', payload: { ok: true } } });
  assert.equal((await reply).ok, true);
});

test('CEF visibility clears DOM focus and delivery is bounded before the SDK attaches', () => {
  const { api, window, receive, blurred } = fixture();
  for (let i = 0; i < 70; ++i) receive({ index: i });
  const received = [];
  api.onMessage = text => received.push(JSON.parse(text));
  assert.equal(received.length, 64);
  assert.equal(received.at(-1).index, 63);
  receive({ kind: 'event', name: 'ui.visibility', payload: { visible: false } });
  assert.equal(blurred.length, 1);
  receive({ kind: 'event', name: 'ui.visibility', payload: { visible: true } });
  assert.equal(blurred.length, 1);
  assert.equal(Object.getOwnPropertyDescriptor(window, '__osfCefReceive').writable, false);
});
