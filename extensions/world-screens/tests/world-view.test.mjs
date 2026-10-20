import assert from 'node:assert/strict';
import test from 'node:test';
import { worldTexturePath, worldPlaceholder } from '../tools/world-texture.mjs';
test('independent named feeds share identical placeholder bytes without sharing identity', () => {
  const paths=Array.from({length:64},(_,i)=>worldTexturePath(`example.mod/board-${i}`));
  assert.equal(new Set(paths).size,64);
  assert.notEqual(worldTexturePath('alpha/screen'),worldTexturePath('beta/screen'));
  const bytes=worldPlaceholder();
  assert.equal(bytes.toString('ascii',0,4),'DDS ');
  assert.equal(bytes.readUInt32LE(12),64);
  assert.equal(bytes.readUInt32LE(16),64);
  assert.deepEqual(bytes,worldPlaceholder());
});
