// Mock file resolution. The historical default pointed at osfui.mock.ts while
// the scaffolder wrote osfui.mock.json and never set `mock:` — so scaffolded
// mocks were silently never loaded. These tests pin the probing order, the
// explicit-path contract, and the never-ships guard.

import assert from 'node:assert/strict';
import { mkdir, mkdtemp, rm, writeFile } from 'node:fs/promises';
import { tmpdir } from 'node:os';
import { resolve } from 'node:path';
import test from 'node:test';

import { loadProject } from '../src/config.mjs';

async function projectFixture(t, { config = '', files = {} } = {}) {
  const root = await mkdtemp(resolve(tmpdir(), 'osfui-mock-'));
  t.after(() => rm(root, { recursive: true, force: true }));
  const view = resolve(root, 'src/views/acme.widgets/panel');
  await mkdir(view, { recursive: true });
  await writeFile(resolve(root, 'osfui.config.ts'), `export default {
    modId: 'acme.widgets',
    views: [{ id: 'panel' }],
    ${config}
  };`);
  await writeFile(resolve(view, 'index.html'), '<main></main>');
  for (const [name, content] of Object.entries(files)) {
    await writeFile(resolve(root, name), content);
  }
  return root;
}

test('a project without any mock loads with mockKind none', async (t) => {
  const root = await projectFixture(t);
  const project = await loadProject(root);
  assert.equal(project.mockPath, null);
  assert.equal(project.mockKind, 'none');
});

test('a JavaScript project config is discovered without a TypeScript config', async (t) => {
  const root = await projectFixture(t);
  await rm(resolve(root, 'osfui.config.ts'));
  await writeFile(resolve(root, 'osfui.config.js'), `export default {
    modId: 'acme.widgets',
    views: [{ id: 'panel' }],
  };`);

  const project = await loadProject(root);
  assert.equal(project.configPath, resolve(root, 'osfui.config.js'));
});

test('the scaffolded osfui.mock.json is found without a mock: setting', async (t) => {
  // The exact shape `npm create osfui` emits: a JSON fixture, no `mock:` key.
  const root = await projectFixture(t, { files: { 'osfui.mock.json': '{"state":{}}' } });
  const project = await loadProject(root);
  assert.equal(project.mockPath, resolve(root, 'osfui.mock.json'));
  assert.equal(project.mockKind, 'json');
});

test('a module mock outranks a JSON fixture', async (t) => {
  const root = await projectFixture(t, {
    files: {
      'osfui.mock.ts': 'export default {};',
      'osfui.mock.json': '{"state":{}}',
    },
  });
  const project = await loadProject(root);
  assert.equal(project.mockPath, resolve(root, 'osfui.mock.ts'));
  assert.equal(project.mockKind, 'module');
});

test('an explicit mock: path is honored and typed by extension', async (t) => {
  const root = await projectFixture(t, {
    config: "mock: 'dev/bridge.mjs',",
    files: {},
  });
  await mkdir(resolve(root, 'dev'), { recursive: true });
  await writeFile(resolve(root, 'dev/bridge.mjs'), 'export default {};');
  const project = await loadProject(root);
  assert.equal(project.mockPath, resolve(root, 'dev/bridge.mjs'));
  assert.equal(project.mockKind, 'module');
});

test('an explicit mock: path that does not exist is an error, not a silent no-op', async (t) => {
  const root = await projectFixture(t, { config: "mock: 'osfui.mock.ts'," });
  await assert.rejects(loadProject(root), /Mock file not found/);
});

test('a mock inside src/views is rejected so it can never ship', async (t) => {
  const root = await projectFixture(t, {
    config: "mock: 'src/views/acme.widgets/mock.ts',",
  });
  await writeFile(resolve(root, 'src/views/acme.widgets/mock.ts'), 'export default {};');
  await assert.rejects(loadProject(root), /never ship/);
});
