import assert from 'node:assert/strict';
import { resolve, sep } from 'node:path';
import test from 'node:test';

import { within, pexFor } from '../src/fsutil.mjs';

test('within checks whole path segments, not a ".." prefix', () => {
  const root = resolve('/project/mod');
  assert.equal(within(root, root), true);
  assert.equal(within(root, resolve(root, 'Scripts/Quest.psc')), true);
  assert.equal(within(root, resolve(root, '..')), false);
  assert.equal(within(root, resolve(root, '../sibling/file')), false);
  // A file literally named "..notes" INSIDE the root is inside — the naive
  // startsWith('..') this replaced said outside, so game-sync's
  // nativeChanged flag never fired for it.
  assert.equal(within(root, resolve(root, '..notes')), true);
  // A SIBLING directory named "..notesdir" is outside.
  assert.equal(within(root, resolve(root, `..${sep}..notesdir${sep}x`)), false);
});

test('pexFor maps Source[/User] sources onto their compiled outputs', () => {
  const mod = resolve('/project/mod');
  assert.equal(
    pexFor(mod, resolve(mod, 'Scripts/Source/User/AcmeOSFUI.psc')),
    resolve(mod, 'Scripts/AcmeOSFUI.pex'),
  );
  assert.equal(
    pexFor(mod, resolve(mod, 'Scripts/Source/Ns/Deep.psc')),
    resolve(mod, 'Scripts/Ns/Deep.pex'),
  );
});
