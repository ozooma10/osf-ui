// The bridge-traffic row model: headlines, details, tones, dedupe keys.
//
// Protocol 2.0 envelopes put routing beside the payload, so every headline is
// derived from `kind` plus the field that kind routes on — never from a field
// inside the payload, which a mod could otherwise use to disguise a row.

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

test('sends and requests read as the endpoint, not the envelope', () => {
  const send = summarize('out', { kind: 'send', name: 'close', payload: {} });
  assert.equal(send.title, 'send · close');
  assert.equal(send.requestId, '');
  assert.equal(send.tone, 'out');

  const request = summarize('out', {
    kind: 'request',
    name: 'settings.set',
    id: 'q1',
    payload: { mod: 'acme', key: 'volume', value: 3 },
  });
  assert.equal(request.title, 'request · settings.set');
  assert.equal(request.detail, 'mod=acme, key=volume, value=3');
  // The correlation id is `id` on the wire; the row model still calls it
  // requestId because that is what the panel column is named.
  assert.equal(request.requestId, 'q1');
  assert.match(request.body, /"name": "settings.set"/);
});

test('state rows name the mod and key, and read the value', () => {
  const row = summarize('in', { kind: 'state', mod: 'acme.mymod', key: 'volume', value: 3 });
  assert.equal(row.title, 'state · acme.mymod/volume');
  assert.equal(row.detail, '3');

  // The locale catalog is the one state value worth summarising rather than
  // spelling out — a few hundred strings would fill the panel.
  const locale = summarize('in', {
    kind: 'state',
    mod: 'osfui',
    key: 'i18n',
    value: { locale: 'de', strings: { a: '1' } },
  });
  assert.equal(locale.title, 'locale · de');
  assert.equal(locale.detail, '1 strings');
});

test('known events get purpose-built headlines', () => {
  assert.equal(
    summarize('in', { kind: 'event', name: 'ui.gamepad', payload: { kind: 'button', button: { id: 0x0100, down: true } } }).title,
    'gamepad · LB');
  assert.equal(
    summarize('in', { kind: 'event', name: 'ui.visibility', payload: { visible: false, reason: 'overlay' } }).title,
    'visibility · hidden');
  assert.equal(
    summarize('in', { kind: 'event', name: 'ui.hotkey', payload: { mod: 'acme', key: 'toggleKey' } }).title,
    'hotkey · toggleKey');
  assert.equal(
    summarize('in', { kind: 'event', name: 'settings.captured', payload: { mod: 'acme', key: 'k', name: 'F9', cancelled: false } }).title,
    'captured · F9');
});

test('a mod event falls back to its own name', () => {
  const row = summarize('in', { kind: 'event', name: 'acme.mymod.scanned', payload: { args: ['3'] } });
  assert.equal(row.title, 'event · acme.mymod.scanned');
  assert.equal(row.detail, 'args=[1]');
});

test('errors take the warn tone whatever the direction says', () => {
  const row = summarize('in', {
    kind: 'error',
    id: 'q1',
    payload: { code: 'mock-unhandled', message: 'No mock response.' },
  });
  assert.equal(row.tone, 'warn');
  assert.equal(row.title, 'error · mock-unhandled');
  assert.equal(row.detail, 'No mock response.');
  assert.equal(row.requestId, 'q1');
});

test('a dev-only protocol complaint reads as one', () => {
  const row = summarize('in', {
    kind: 'event',
    name: 'osfui.debug.error',
    payload: { code: 'wrong-endpoint-kind', message: "'settings.set' is a request endpoint" },
  });
  assert.equal(row.title, 'protocol · wrong-endpoint-kind');
  assert.equal(row.detail, "'settings.set' is a request endpoint");
});

test('replies and the handshake are legible without a name', () => {
  assert.equal(summarize('in', { kind: 'reply', id: 'q1', payload: { value: 7 } }).title, 'reply');
  assert.equal(summarize('in', { kind: 'ready', payload: { game: 'Starfield', version: '2.0.0' } }).title, 'ready');
});

test('an envelope with no kind falls back to a payload preview', () => {
  const row = summarize('in', { payload: { a: 1 } });
  assert.equal(row.title, '(no kind)');
  assert.equal(row.detail, 'a=1');
});

test('harness notes are note-toned, bodyless rows', () => {
  const row = summarize('in', 'Mock: fixtures.js', '');
  assert.equal(row.title, 'Mock: fixtures.js');
  assert.equal(row.body, '');
  assert.equal(row.tone, 'note');
  assert.equal(summarize('in', 'Bridge disabled', 'warn').tone, 'warn');
});

test('the dedupe key folds repeats but separates directions and details', () => {
  const push = (value) => summarize('in', { kind: 'state', mod: 'acme', key: 'k', value }).key;
  assert.equal(push(1), push(1));
  assert.notEqual(push(1), push(2));
  const send = { kind: 'send', name: 'close', payload: {} };
  assert.notEqual(summarize('out', send).key, summarize('in', send).key);
});
