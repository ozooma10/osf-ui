// The tool-strip model: registration normalization, patches, cycle order.

import assert from 'node:assert/strict';
import test from 'node:test';

import { applyPatch, nextCycleValue, normalizeTools } from '../src/browser/tools-model.js';

test('valid specs pass through, defaulted per kind', () => {
  const { tools, dropped } = normalizeTools([
    { id: 'reset', kind: 'button', label: 'Reset values', title: 'Clear stored values' },
    { id: 'fixtures', kind: 'toggle', label: 'Sample views' },
    { id: 'health', kind: 'cycle', label: 'Health', options: ['clean', 'errors'] },
    { id: 'locale', kind: 'select', label: 'Locale', options: [{ value: 'en', label: 'en (authored)' }, 'de'], value: 'de' },
  ]);
  assert.equal(dropped.length, 0);
  assert.equal(tools.length, 4);
  assert.equal(tools[1].value, false);
  assert.equal(tools[2].value, 'clean');
  assert.equal(tools[3].value, 'de');
  assert.deepEqual(tools[3].options[0], { value: 'en', label: 'en (authored)' });
});

test('bad ids, kinds, missing labels, and optionless cycles are dropped', () => {
  const { tools, dropped } = normalizeTools([
    { id: 'UPPER', kind: 'button', label: 'x' },
    { id: 'ok-1', kind: 'dial', label: 'x' },
    { id: 'ok-2', kind: 'button' },
    { id: 'ok-3', kind: 'cycle', label: 'x' },
    { id: 'ok-4', kind: 'select', label: 'x', options: [42] },
    null,
    'nonsense',
    { id: 'fine', kind: 'button', label: 'Fine' },
  ]);
  assert.deepEqual(tools.map((tool) => tool.id), ['fine']);
  assert.equal(dropped.length, 7);
});

test('duplicate ids: last registration wins', () => {
  const { tools } = normalizeTools([
    { id: 'x', kind: 'button', label: 'first' },
    { id: 'x', kind: 'button', label: 'second' },
  ]);
  assert.equal(tools.length, 1);
  assert.equal(tools[0].label, 'second');
});

test('a cycle/select value outside its options snaps to the first option', () => {
  const { tools } = normalizeTools([
    { id: 'c', kind: 'cycle', label: 'c', options: ['a', 'b'], value: 'zzz' },
  ]);
  assert.equal(tools[0].value, 'a');
});

test('applyPatch merges known fields for the matching id only', () => {
  const { tools } = normalizeTools([
    { id: 'health', kind: 'cycle', label: 'Health', options: ['clean', 'errors'] },
    { id: 'reset', kind: 'button', label: 'Reset' },
  ]);
  const patched = applyPatch(tools, 'health', { value: 'errors', active: true, label: 'Health!' });
  assert.equal(patched[0].value, 'errors');
  assert.equal(patched[0].active, true);
  assert.equal(patched[0].label, 'Health!');
  assert.deepEqual(patched[1], tools[1]);
  // Off-list values and unknown ids are ignored.
  assert.equal(applyPatch(patched, 'health', { value: 'bogus' })[0].value, 'errors');
  assert.deepEqual(applyPatch(patched, 'ghost', { value: 'x' }), patched);
  assert.deepEqual(applyPatch(patched, 'health', null), patched);
});

test('toggle patches coerce to boolean', () => {
  const { tools } = normalizeTools([{ id: 't', kind: 'toggle', label: 't' }]);
  assert.equal(applyPatch(tools, 't', { value: true })[0].value, true);
  assert.equal(applyPatch(tools, 't', { value: 'yes' })[0].value, false);
});

test('nextCycleValue wraps around', () => {
  const { tools } = normalizeTools([
    { id: 'c', kind: 'cycle', label: 'c', options: ['a', 'b', 'c'], value: 'c' },
  ]);
  assert.equal(nextCycleValue(tools[0]), 'a');
  assert.equal(nextCycleValue({ ...tools[0], value: 'a' }), 'b');
});
