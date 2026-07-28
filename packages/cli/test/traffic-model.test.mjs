// The bridge-traffic row model: headlines, details, tones, dedupe keys.

import assert from 'node:assert/strict';
import test from 'node:test';

import { preview, summarize } from '../src/browser/traffic-model.js';

test('preview flattens objects to key=value and counts nesting', () => {
  assert.equal(preview({ mod: 'acme', key: 'volume' }), 'mod=acme, key=volume');
  assert.equal(preview({ strings: { a: 1, b: 2 }, list: [1, 2, 3] }), 'strings={2}, list=[3]');
  assert.equal(preview('plain text'), 'plain text');
  assert.equal(preview(42), '42');
  assert.equal(preview({ text: 'x'.repeat(200) }).length, 140);
});

test('commands read as the command, not the envelope', () => {
  const row = summarize('out', {
    type: 'ui.command',
    requestId: 'r1',
    payload: { command: 'settings.get', mod: 'acme' },
  });
  assert.equal(row.title, 'command · settings.get');
  assert.equal(row.detail, 'mod=acme');
  assert.equal(row.requestId, 'r1');
  assert.equal(row.tone, 'out');
  assert.match(row.body, /"command": "settings.get"/);
});

test('known push types get purpose-built headlines', () => {
  assert.equal(summarize('in', { type: 'data.state', payload: { key: 'volume', value: 3 } }).title,
    'state · volume');
  assert.equal(summarize('in', { type: 'data.state', payload: { key: 'volume', value: 3 } }).detail, '3');
  assert.equal(summarize('in', { type: 'i18n.data', payload: { locale: 'de', strings: { a: '1' } } }).detail,
    '1 strings');
  assert.equal(summarize('in', { type: 'ui.gamepad', payload: { kind: 'button', button: { id: 0x0100, down: true } } }).title,
    'gamepad · LB');
  assert.equal(summarize('in', { type: 'ui.visibility', payload: { visible: false, reason: 'overlay' } }).title,
    'visibility · hidden');
  assert.equal(summarize('in', { type: 'ui.hotkey', payload: { mod: 'acme', key: 'toggleKey' } }).title,
    'hotkey · toggleKey');
});

test('errors take the warn tone whatever the direction says', () => {
  const row = summarize('in', { type: 'ui.error', payload: { code: 'mock-unhandled', message: 'No mock response.' } });
  assert.equal(row.tone, 'warn');
  assert.equal(row.title, 'error · mock-unhandled');
  assert.equal(row.detail, 'No mock response.');
});

test('unknown types fall back to type plus a payload preview', () => {
  const row = summarize('in', { payload: { a: 1 } });
  assert.equal(row.title, '(untyped)');
  const custom = summarize('in', { type: 'acme.thing', payload: { a: 1 } });
  assert.equal(custom.title, 'acme.thing');
  assert.equal(custom.detail, 'a=1');
});

test('harness notes are note-toned, bodyless rows', () => {
  const row = summarize('in', 'Mock: fixtures.js', '');
  assert.equal(row.title, 'Mock: fixtures.js');
  assert.equal(row.body, '');
  assert.equal(row.tone, 'note');
  assert.equal(summarize('in', 'Bridge disabled', 'warn').tone, 'warn');
});

test('the dedupe key folds repeats but separates directions and details', () => {
  const push = (value) => summarize('in', { type: 'data.state', payload: { key: 'k', value } }).key;
  assert.equal(push(1), push(1));
  assert.notEqual(push(1), push(2));
  const command = { type: 'ui.command', payload: { command: 'close' } };
  assert.notEqual(summarize('out', command).key, summarize('in', command).key);
});
