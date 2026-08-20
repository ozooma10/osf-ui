import assert from 'node:assert/strict';
import test from 'node:test';

import { applyPatch, nextCycleValue, normalizeTools } from '../src/browser/tools-model.js';

test('generated-project button and toggle controls normalize and patch', () => {
  const { tools, dropped } = normalizeTools([
    { id: 'push-event', kind: 'button', label: 'Push event' },
    { id: 'backend-enabled', kind: 'toggle', label: 'Backend enabled', value: true },
    { id: 'locale', kind: 'select', label: 'Locale', options: ['en', 'de'], value: 'de' },
  ]);
  assert.deepEqual(dropped, []);
  assert.deepEqual(tools.map(({ kind }) => kind), ['button', 'toggle', 'select']);
  assert.equal(tools[1].value, true);
  assert.equal(applyPatch(tools, 'backend-enabled', { value: false })[1].value, false);
});

test('invalid controls are dropped and cycle values wrap', () => {
  const { tools, dropped } = normalizeTools([
    { id: 'BAD', kind: 'button', label: 'Bad' },
    { id: 'mode', kind: 'cycle', label: 'Mode', options: ['a', 'b'], value: 'b' },
  ]);
  assert.deepEqual(dropped, ['BAD']);
  assert.equal(nextCycleValue(tools[0]), 'a');
});
