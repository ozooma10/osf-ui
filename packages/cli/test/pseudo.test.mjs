// The pseudo-locale transform. Brackets are the contract: unbracketed text on
// screen is a string that never went through the localization path.

import assert from 'node:assert/strict';
import test from 'node:test';

import { pseudoize, pseudoizeStrings } from '../src/browser/pseudo.js';

test('accents, pads, and brackets a string', () => {
  const out = pseudoize('Save');
  assert.match(out, /^\[Šåṽé·+\]$/);
});

test('padding grows with length but clamps at 12', () => {
  const short = pseudoize('ab');
  assert.match(short, /·\]$/);
  assert.equal(short.match(/·+/)[0].length, 1);
  const long = pseudoize('x'.repeat(200));
  assert.equal(long.match(/·+/)[0].length, 12);
});

test('non-strings and empty strings pass through', () => {
  assert.equal(pseudoize(''), '');
  assert.equal(pseudoize(42), 42);
  assert.equal(pseudoize(null), null);
  assert.equal(pseudoize(undefined), undefined);
});

test('non-letters survive untouched inside the brackets', () => {
  assert.match(pseudoize('HP: 100%'), /^\[ĤÞ: 100%·+\]$/);
});

test('pseudoizeStrings maps a catalog', () => {
  const out = pseudoizeStrings({ a: 'One', b: '' });
  assert.match(out.a, /^\[Øñé·+\]$/);
  assert.equal(out.b, '');
  assert.deepEqual(pseudoizeStrings(null), {});
});
