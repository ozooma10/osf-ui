// The scenario engine and scenario overlay — the pure core of the
// programmable mock. DOM-touching parts (installMock) are covered by the
// server-level test in toolchain.test.mjs.

import assert from 'node:assert/strict';
import test from 'node:test';

import { createScenarioHandler, resolveScenario } from '../src/browser/mock-runtime.js';

const META = { modId: 'acme.widgets' };

function harness(scenario) {
  const replies = [];
  const reports = [];
  const engine = createScenarioHandler(resolveScenario(scenario, null), META);
  const run = (command, payload = {}, requestId = 'r1') =>
    engine(command, payload, requestId, {
      reply: (type, replyPayload) => replies.push({ type, payload: replyPayload }),
      report: (direction, message, level) => reports.push({ direction, message, level }),
    });
  return { run, replies, reports };
}

test('a plain request value replies mock.result', async () => {
  const { run, replies } = harness({ requests: { 'acme.widgets.getWeight': { weight: 42.5 } } });
  await run('acme.widgets.getWeight');
  assert.deepEqual(replies, [{ type: 'mock.result', payload: { weight: 42.5 } }]);
});

test('a $type request controls the reply type', async () => {
  const { run, replies } = harness({
    requests: { 'acme.widgets.scan': { $type: 'acme.widgets.scanned', payload: { hits: 3 } } },
  });
  await run('acme.widgets.scan');
  assert.deepEqual(replies, [{ type: 'acme.widgets.scanned', payload: { hits: 3 } }]);
});

test('a function request receives the payload and may be async', async () => {
  const { run, replies } = harness({
    requests: {
      echo: async (payload) => ({ got: payload.value }),
    },
  });
  await run('echo', { value: 7 });
  assert.deepEqual(replies, [{ type: 'mock.result', payload: { got: 7 } }]);
});

test('a function request returning $type controls the reply type', async () => {
  const { run, replies } = harness({
    requests: { probe: () => ({ $type: 'probe.data', payload: { ok: true } }) },
  });
  await run('probe');
  assert.deepEqual(replies, [{ type: 'probe.data', payload: { ok: true } }]);
});

test('papyrus requests are looked up under the papyrus. prefix', async () => {
  const { run, replies } = harness({
    requests: { 'papyrus.GetCount': { count: 2 } },
  });
  await run('ui.papyrusRequest', { request: 'GetCount' });
  assert.deepEqual(replies, [{ type: 'papyrus.result', payload: { value: { count: 2 } } }]);
});

test('an unknown papyrus request errors instead of hanging the caller', async () => {
  const { run, replies } = harness({});
  await run('ui.papyrusRequest', { request: 'Nope' });
  assert.equal(replies[0].type, 'ui.error');
  assert.equal(replies[0].payload.code, 'mock-unhandled');
});

test('i18n.get answers from the active locale catalog', async () => {
  const { run, replies } = harness({
    locale: 'de',
    locales: { de: { title: 'Beispiel' } },
  });
  await run('i18n.get', {});
  assert.deepEqual(replies, [{
    type: 'i18n.data',
    payload: { mod: 'acme.widgets', locale: 'de', strings: { title: 'Beispiel' } },
  }]);
});

test('built-in verbs ack without configuration', async () => {
  const { run, replies } = harness({});
  await run('view.ready');
  assert.equal(replies[0].type, 'ui.result');
  assert.equal(replies[0].payload.ok, true);
});

test('anything else is mock-unhandled: reply with a requestId, warn without', async () => {
  const { run, replies, reports } = harness({});
  await run('acme.widgets.mystery');
  assert.equal(replies[0].type, 'ui.error');
  assert.equal(replies[0].payload.code, 'mock-unhandled');
  const silent = harness({});
  await silent.run('acme.widgets.mystery', {}, '');
  assert.equal(silent.replies.length, 0);
  assert.equal(silent.reports[0].level, 'warn');
  assert.equal(reports.length, 0);
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
