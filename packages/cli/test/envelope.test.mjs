import assert from 'node:assert/strict';
import test from 'node:test';

import { NATIVE_TO_WEB_KINDS, parseNativeEnvelope } from '../src/browser/envelope.js';

test('the manual injector accepts every native-to-web 2.0 envelope kind', () => {
  assert.deepEqual([...NATIVE_TO_WEB_KINDS], ['ready', 'state', 'event', 'reply', 'error']);
  for (const kind of NATIVE_TO_WEB_KINDS) {
    assert.equal(parseNativeEnvelope(JSON.stringify({ kind })).kind, kind);
  }
});

test('the manual injector rejects legacy type envelopes and web-to-native kinds', () => {
  assert.throws(() => parseNativeEnvelope('{"type":"ui.event"}'), /message\.kind/);
  assert.throws(() => parseNativeEnvelope('{"kind":"send"}'), /message\.kind/);
  assert.throws(() => parseNativeEnvelope('[]'), /message\.kind/);
});
