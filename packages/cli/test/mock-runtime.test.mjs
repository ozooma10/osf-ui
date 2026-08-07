// The scenario engine and scenario overlay — the pure core of the
// programmable mock. DOM-touching parts (installMock) are covered by the
// server-level test in toolchain.test.mjs.
//
// Protocol 2.0: the engine is handed the envelope's KIND alongside its name, so
// a send and a request naming the same endpoint are different events. Requests
// settle through io.resolve / io.reject; a send has nothing to settle and gets
// io.reportProtocolFault when the harness cannot place it.

import assert from 'node:assert/strict';
import { readFile } from 'node:fs/promises';
import { resolve } from 'node:path';
import test from 'node:test';

import {
  createScenarioHandler,
  PLATFORM_REQUESTS,
  PLATFORM_SENDS,
  resolveScenario,
} from '../src/browser/mock-runtime.js';

const META = { modId: 'acme.widgets', qualifiedId: 'acme.widgets/panel' };

function harness(scenario) {
  const settled = [];
  const faults = [];
  const reports = [];
  const engine = createScenarioHandler(resolveScenario(scenario, null), META);
  const io = {
    resolve: (payload) => settled.push({ ok: true, payload }),
    reject: (code, message) => settled.push({ ok: false, code, message }),
    reportProtocolFault: (code, message) => faults.push({ code, message }),
    // Exercise the frozen compatibility spelling alongside the canonical callback.
    surface: (code, message) => faults.push({ code, message }),
    report: (direction, message, level) => reports.push({ direction, message, level }),
  };
  return {
    request: (name, payload = {}) => engine('request', name, payload, io),
    send: (name, payload = {}) => engine('send', name, payload, io),
    settled,
    faults,
    reports,
  };
}

test('a plain request value resolves as the reply payload', async () => {
  const h = harness({ requests: { 'acme.widgets.getWeight': { weight: 42.5 } } });
  await h.request('acme.widgets.getWeight');
  assert.deepEqual(h.settled, [{ ok: true, payload: { weight: 42.5 } }]);
});

test('a $payload wrapper nests the reply without inventing a reply type', async () => {
  // 1.x let a mock pick the reply MESSAGE TYPE with `$type`. A reply has no
  // type any more — it is just a payload — so the wrapper only survives as a
  // way to nest one.
  const h = harness({
    requests: { 'acme.widgets.scan': { $payload: { hits: 3 } } },
  });
  await h.request('acme.widgets.scan');
  assert.deepEqual(h.settled, [{ ok: true, payload: { hits: 3 } }]);
});

test('a function request receives the payload and may be async', async () => {
  const h = harness({
    requests: { echo: async (payload) => ({ got: payload.value }) },
  });
  await h.request('echo', { value: 7 });
  assert.deepEqual(h.settled, [{ ok: true, payload: { got: 7 } }]);
});

test('papyrus requests are looked up under the papyrus. prefix', async () => {
  const h = harness({ requests: { 'papyrus.GetCount': { count: 2 } } });
  await h.request('papyrus.request', { name: 'GetCount' });
  assert.deepEqual(h.settled, [{ ok: true, payload: { value: { count: 2 } } }]);
});

test('an unknown papyrus request rejects instead of hanging the caller', async () => {
  const h = harness({});
  await h.request('papyrus.request', { name: 'Nope' });
  assert.equal(h.settled[0].ok, false);
  assert.equal(h.settled[0].code, 'mock-unhandled');
});

test('sending to papyrus.request reports a wrong-kind protocol fault', async () => {
  const h = harness({ requests: { 'papyrus.GetCount': 2 } });
  await h.send('papyrus.request', { name: 'GetCount' });
  assert.equal(h.settled.length, 0);
  assert.equal(h.faults[0].code, 'unknown-endpoint');
});

test('built-in request endpoints resolve without configuration', async () => {
  const h = harness({});
  await h.request('menu.open', { view: 'acme.widgets/panel' });
  assert.deepEqual(h.settled, [{ ok: true, payload: {} }]);
});

test('the endpoint registry mirrors the current native runtime', async () => {
  const [runtime, settings] = await Promise.all([
    readFile(resolve(import.meta.dirname, '../../../src/runtime/Runtime.cpp'), 'utf8'),
    readFile(resolve(import.meta.dirname, '../../../src/runtime/SettingsModule.cpp'), 'utf8'),
  ]);
  const names = (source, kind) => [
    ...source.matchAll(new RegExp(`Register${kind}\\("([^"]+)"`, 'g')),
  ].map((match) => match[1]);
  assert.deepEqual(
    [...PLATFORM_SENDS].sort(),
    ['osfui.hello', ...names(runtime, 'Send')].sort(),
  );
  assert.deepEqual(
    [...PLATFORM_REQUESTS].sort(),
    [...names(runtime, 'Request'), ...names(settings, 'Request')].sort(),
  );
});

test('platform requests return protocol-shaped stand-ins', async () => {
  const settings = harness({});
  await settings.request('settings.set', {
    mod: 'acme.widgets', key: 'toggleKey', value: 'F9',
  });
  assert.deepEqual(settings.settled, [{
    ok: true,
    payload: { mod: 'acme.widgets', key: 'toggleKey', value: 'F9' },
  }]);

  const capture = harness({});
  await capture.request('settings.captureKey', { mod: 'acme.widgets', key: 'toggleKey' });
  assert.deepEqual(capture.settled[0], {
    ok: true,
    payload: { armed: true, mod: 'acme.widgets', key: 'toggleKey' },
  });

  const game = harness({});
  await game.request('game.get');
  assert.deepEqual(game.settled[0], {
    ok: true,
    payload: { calendar: { available: false } },
  });
});

test('platform request authority and papyrus.call security match the runtime', async () => {
  const settings = harness({});
  await settings.request('settings.set', { mod: 'another.mod', key: 'toggleKey', value: 'F9' });
  assert.equal(settings.settled[0].code, 'forbidden');

  const papyrus = harness({});
  await papyrus.send('papyrus.call', { script: 'OsFuI', function: 'SetString' });
  assert.equal(papyrus.faults[0].code, 'forbidden');

  const handoff = harness({});
  await handoff.send('osfui.handoffRetry');
  assert.equal(handoff.faults[0].code, 'forbidden');
});

test('a built-in SEND is accepted silently — there is nothing to settle', async () => {
  const h = harness({});
  await h.send('view.ready');
  assert.equal(h.settled.length, 0);
  assert.equal(h.faults.length, 0);
});

test('requesting a send endpoint is a kind mismatch, not a missing mock', async () => {
  const h = harness({});
  await h.request('view.ready');
  assert.equal(h.settled[0].ok, false);
  assert.equal(h.settled[0].code, 'wrong-endpoint-kind');
});

test('an unhandled request rejects; an unhandled send reports a protocol fault', async () => {
  const h = harness({});
  await h.request('acme.widgets.mystery');
  assert.equal(h.settled[0].ok, false);
  assert.equal(h.settled[0].code, 'mock-unhandled');

  // The send has no promise to reject, so the only honest channel is the
  // offending page's own console — which is exactly what osfui.debug.error is
  // for. Dropping it silently is the 1.x behavior 2.0 set out to remove.
  const send = harness({});
  await send.send('acme.widgets.mystery');
  assert.equal(send.settled.length, 0);
  assert.equal(send.faults.length, 1);
  assert.equal(send.faults[0].code, 'unknown-endpoint');
});

test('the deprecated io.surface-only fixture shape remains compatible', async () => {
  const faults = [];
  const engine = createScenarioHandler(resolveScenario({}, null), META);
  await engine('send', 'acme.widgets.mystery', {}, {
    resolve() {},
    reject() {},
    surface: (code, message) => faults.push({ code, message }),
    report() {},
  });
  assert.equal(faults.length, 1);
  assert.equal(faults[0].code, 'unknown-endpoint');
});

test('a scenario answer to a one-way send is reported as an authoring mistake', async () => {
  const h = harness({ requests: { 'acme.widgets.doIt': { ok: true } } });
  await h.send('acme.widgets.doIt');
  assert.equal(h.settled.length, 0);
  assert.equal(h.reports[0].level, 'warn');
});

test('scenarios overlay the base fields per key', () => {
  const mock = {
    state: { a: 1, b: 2 },
    locale: 'en',
    requests: { ping: 'base' },
    scenarios: {
      broken: { state: { b: 3 }, requests: { ping: 'broken' }, locale: 'de' },
    },
  };
  const base = resolveScenario(mock, null);
  assert.deepEqual(base.state, { a: 1, b: 2 });
  assert.equal(base.locale, 'en');
  assert.deepEqual(base.scenarioNames, ['broken']);
  const overlaid = resolveScenario(mock, 'broken');
  assert.deepEqual(overlaid.state, { a: 1, b: 3 });
  assert.equal(overlaid.requests.ping, 'broken');
  assert.equal(overlaid.locale, 'de');
  assert.equal(overlaid.scenario, 'broken');
  // Unknown scenario names fall back to the base.
  assert.deepEqual(resolveScenario(mock, 'nope').state, { a: 1, b: 2 });
});

test('resolveScenario tolerates junk input', () => {
  for (const junk of [null, undefined, 42, 'text', []]) {
    const scenario = resolveScenario(junk, 'anything');
    assert.deepEqual(scenario.state, {});
    assert.deepEqual(scenario.requests, {});
    assert.equal(scenario.locale, 'en');
  }
});
