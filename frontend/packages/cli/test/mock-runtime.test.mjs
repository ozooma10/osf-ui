import assert from 'node:assert/strict';
import { readFile } from 'node:fs/promises';
import { resolve } from 'node:path';
import test from 'node:test';

import {
  createEndpointHandler,
  installMock,
  PLATFORM_REQUESTS,
  PLATFORM_SENDS,
  resolveScenario,
} from '../src/browser/mock-runtime.js';

const META = { modId: 'acme.widgets', qualifiedId: 'acme.widgets/panel' };

function endpointHarness(mock = {}) {
  const settled = [];
  const surfaced = [];
  const reports = [];
  const endpoint = createEndpointHandler(resolveScenario(mock, ''), META);
  const io = {
    resolve: (payload) => settled.push({ ok: true, payload }),
    reject: (code, message) => settled.push({ ok: false, code, message }),
    surface: (code, message) => surfaced.push({ code, message }),
    report: (direction, message, level) => reports.push({ direction, message, level }),
  };
  return {
    request: (name, payload = {}) => endpoint('request', name, payload, io),
    send: (name, payload = {}) => endpoint('send', name, payload, io),
    settled,
    surfaced,
    reports,
  };
}

test('configured request replies stay raw, including every falsy scalar', async () => {
  for (const value of [false, 0, '', null]) {
    const mock = { requests: { inspect: value } };
    const local = endpointHarness(mock);
    await local.request('inspect', { args: [] });
    assert.deepEqual(local.settled, [{ ok: true, payload: value }]);

    const qualified = endpointHarness(mock);
    await qualified.request('acme.widgets.inspect', { args: [] });
    assert.deepEqual(qualified.settled, [{ ok: true, payload: value }]);
  }
});

test('endpoint functions receive the generic object payload', async () => {
  const h = endpointHarness({
    requests: { greet: ({ args }) => String(args[0]) },
  });
  await h.request('greet', { args: ['Constellation'] });
  assert.deepEqual(h.settled, [{ ok: true, payload: 'Constellation' }]);
});

test('platform registry mirrors current native registrations and excludes removed helpers', async () => {
  const [runtime, settings] = await Promise.all([
    readFile(resolve(import.meta.dirname, '../../../../src/Bridge/RuntimeBridge.cpp'), 'utf8'),
    readFile(resolve(import.meta.dirname, '../../../../src/Settings/SettingsModule.cpp'), 'utf8'),
  ]);
  const names = (source, kind) => [
    ...source.matchAll(new RegExp(`Register${kind}\\(\"([^\"]+)\"`, 'g')),
  ].map((match) => match[1]);
  assert.deepEqual([...PLATFORM_SENDS].sort(), ['osfui.hello', ...names(runtime, 'Send')].sort());
  assert.deepEqual(
    [...PLATFORM_REQUESTS].sort(),
    [...names(runtime, 'Request'), ...names(settings, 'Request')].sort(),
  );
  assert.equal(PLATFORM_SENDS.has('papyrus.send'), false);
  assert.equal(PLATFORM_REQUESTS.has('papyrus.request'), false);
});

test('wrong-kind and unknown endpoints fail through the matching protocol channel', async () => {
  const requestSend = endpointHarness();
  await requestSend.request('papyrus.call');
  assert.equal(requestSend.settled[0].code, 'wrong-endpoint-kind');

  const sendRequest = endpointHarness();
  await sendRequest.send('ping');
  assert.equal(sendRequest.surfaced[0].code, 'wrong-endpoint-kind');

  const unknownRequest = endpointHarness();
  await unknownRequest.request('missing');
  assert.equal(unknownRequest.settled[0].code, 'mock-unhandled');

  const unknownSend = endpointHarness();
  await unknownSend.send('missing');
  assert.equal(unknownSend.surfaced[0].code, 'unknown-endpoint');
});

test('papyrus.call keeps only the advanced GLOBAL escape hatch', async () => {
  for (const script of ['OSFUI', 'osfui_settings', 'OsFuI_ViEw']) {
    const h = endpointHarness();
    await h.send('papyrus.call', { script, function: 'Unsafe', args: [] });
    assert.equal(h.surfaced[0].code, 'forbidden');
  }
  const own = endpointHarness();
  await own.send('papyrus.call', { script: 'AcmeWidgets', function: 'Refresh', args: [] });
  assert.equal(own.surfaced.length, 0);
});

test('a mock import error still installs hello, ready, and request handling', { concurrency: false }, async (t) => {
  const saved = {
    window: globalThis.window,
    location: globalThis.location,
    parent: globalThis.parent,
  };
  t.after(() => {
    for (const [key, value] of Object.entries(saved)) {
      if (value === undefined) delete globalThis[key];
      else globalThis[key] = value;
    }
  });

  const listeners = [];
  globalThis.location = { search: '', origin: 'http://osfui.local' };
  globalThis.parent = { postMessage() {} };
  globalThis.window = {
    localStorage: {
      setItem() {},
      removeItem() {},
    },
    addEventListener(_name, listener) { listeners.push(listener); },
  };

  const delivered = [];
  const statuses = [];
  let bridgeHandler = null;
  const harness = {
    meta: {
      ...META,
      nativeBridge: true,
      version: '2.0.0',
      bridgeVersion: '2',
    },
    source: 'osfui-harness',
    report() {},
    deliver(message) { delivered.push(message); },
    flush() { return true; },
    setHandler(handler) { bridgeHandler = handler; },
    ready() {},
    status(ok, message) { statuses.push({ ok, message }); },
  };

  await installMock(harness, {}, new Error('mock syntax exploded'));
  assert.equal(typeof bridgeHandler, 'function');
  bridgeHandler(JSON.stringify({ kind: 'send', name: 'osfui.hello', payload: {} }));
  bridgeHandler(JSON.stringify({ kind: 'request', name: 'ping', id: 'q1', payload: {} }));
  await new Promise((resolveMicrotasks) => setImmediate(resolveMicrotasks));

  assert.ok(delivered.some((message) => message.kind === 'ready'));
  assert.ok(delivered.some((message) => message.kind === 'event' &&
    message.payload?.code === 'mock-load-error'));
  assert.ok(delivered.some((message) => message.kind === 'reply' && message.id === 'q1'));
  assert.equal(statuses.at(-1).ok, false);
  assert.match(statuses.at(-1).message, /mock syntax exploded/);
});

test('scenario overlays keep base state and local request names', () => {
  const scenario = resolveScenario({
    state: { count: 1, enabled: true },
    requests: { getState: { count: 1 } },
    scenarios: { empty: { state: { count: 0 }, requests: { getState: null } } },
  }, 'empty');
  assert.deepEqual(scenario.state, { count: 0, enabled: true });
  assert.equal(scenario.requests.getState, null);
});
