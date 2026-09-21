import assert from 'node:assert/strict';
import { mkdir, mkdtemp, rm, writeFile } from 'node:fs/promises';
import { tmpdir } from 'node:os';
import { resolve } from 'node:path';
import test from 'node:test';

import { loadProject, manifestFor } from '../src/config.mjs';

async function fixture(t, view) {
  const root = await mkdtemp(resolve(tmpdir(), 'osfui-world-manifest-'));
  t.after(() => rm(root, { recursive: true, force: true }));
  const source = resolve(root, 'src/views/example.mod/screen');
  await mkdir(source, { recursive: true });
  await writeFile(resolve(source, 'index.html'), '<p>World display</p>');
  await writeFile(resolve(root, 'osfui.config.mjs'),
    `export default ${JSON.stringify({ modId: 'example.mod', view: { id: 'screen', ...view } })};`);
  return root;
}

test('world manifest preserves binding and forces passive opaque presentation', async (t) => {
  const root = await fixture(t, {
    kind: 'world', placeholderSize: 1000, width: 1600, height: 900,
    transparent: true, capturesInput: true, pausesGame: true, openOnStart: true, order: 22,
  });
  const project = await loadProject(root, 'build');
  assert.deepEqual(manifestFor(project.views[0]), {
    manifestVersion: 1, title: 'example.mod/screen', description: '', entry: 'index.html',
    kind: 'world', width: 1600, height: 900, placeholderSize: 1000,
    transparent: false, capturesInput: false, pausesGame: false,
    openOnStart: false, order: 0, debugOnly: false,
  });
});

test('world signature and dimensions reject coercion, clipping, and integer overflow', async (t) => {
  for (const placeholderSize of [undefined, null, true, '1000', 1000.5, -1, 255, 256, 512, 1024, 2048, 4096, 4097, 4294968296]) {
    const root = await fixture(t, { kind: 'world', placeholderSize });
    await assert.rejects(loadProject(root, 'build'), /placeholderSize/);
  }
  for (const key of ['width', 'height']) {
    for (const size of [null, true, '900', 0, -1, 1.5, 4097, 4294968196]) {
      const root = await fixture(t, { kind: 'world', placeholderSize: 1000, [key]: size });
      await assert.rejects(loadProject(root, 'build'), new RegExp(key));
    }
  }
});

test('world dimensions default and valid boundary dimensions survive', async (t) => {
  for (const placeholderSize of [257, 1000, 4095]) {
    const root = await fixture(t, { kind: 'world', placeholderSize, width: 1, height: 4096 });
    const project = await loadProject(root, 'build');
    assert.equal(project.views[0].width, 1);
    assert.equal(project.views[0].height, 4096);
  }
  const root = await fixture(t, { kind: 'world', placeholderSize: 1000 });
  const project = await loadProject(root, 'build');
  assert.equal(project.views[0].width, 1600);
  assert.equal(project.views[0].height, 900);
});
