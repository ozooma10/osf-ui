import assert from 'node:assert/strict';
import { mkdtemp, readFile, readdir, rm, writeFile, mkdir } from 'node:fs/promises';
import { tmpdir } from 'node:os';
import { dirname, resolve } from 'node:path';
import { spawnSync } from 'node:child_process';
import { fileURLToPath } from 'node:url';
import test from 'node:test';

const HERE = dirname(fileURLToPath(import.meta.url));
const CLI = resolve(HERE, '..', 'src', 'cli.mjs');
const TEMPLATES = resolve(HERE, '..', 'templates', 'projects');

async function create(t, integration = 'papyrus', extra = []) {
  const parent = await mkdtemp(resolve(tmpdir(), 'create-osfui-'));
  const root = resolve(parent, 'project');
  t.after(() => rm(parent, { recursive: true, force: true }));
  const result = spawnSync(process.execPath, [CLI, root, '--yes', '--mod-id', 'acme.widgets',
    '--view', 'panel', '--surface', 'menu', '--integration', integration, ...extra],
  { encoding: 'utf8' });
  return { root, result };
}

test('stores only the two web-view project templates', async () => {
  const entries = [];
  for (const entry of await readdir(TEMPLATES, { withFileTypes: true })) {
    if (!entry.isDirectory()) continue;
    const content = await readdir(resolve(TEMPLATES, entry.name), {
      recursive: true,
      withFileTypes: true,
    });
    if (content.some((item) => item.isFile())) entries.push(entry.name);
  }
  entries.sort();
  assert.deepEqual(entries, ['menu-native', 'menu-papyrus']);
});

for (const integration of ['papyrus', 'native']) {
  test(`creates a ${integration} view at the OSF/UI path`, async (t) => {
    const { root, result } = await create(t, integration);
    assert.equal(result.status, 0, result.stderr);
    const view = resolve(root, 'mod/Data/SFSE/Plugins/OSF/UI/views/acme.widgets/panel');
    assert.deepEqual((await readdir(view)).sort(), ['index.html', 'main.js', 'manifest.json', 'style.css']);
    const manifest = JSON.parse(await readFile(resolve(view, 'manifest.json'), 'utf8'));
    assert.equal(manifest.manifestVersion, 1);
    assert.equal(manifest.targetVersion, undefined);
    assert.equal(manifest.hub, undefined);
  });
}

test('copies only the OSF UI 2 public surface', async (t) => {
  const papyrus = await create(t, 'papyrus');
  assert.equal(papyrus.result.status, 0, papyrus.result.stderr);
  assert.deepEqual((await readdir(resolve(papyrus.root, 'tools/papyrus'))).sort(),
    ['OSFUI.psc', 'OSFUI_View.psc']);

  const native = await create(t, 'native');
  assert.equal(native.result.status, 0, native.result.stderr);
  assert.deepEqual(await readdir(resolve(native.root, 'native/include')), ['OSFUI_Views.h']);
  const source = await readFile(resolve(native.root, 'native/src/main.cpp'), 'utf8');
  assert.doesNotMatch(source, /OSFUI_Settings|OSFUI_Diagnostics|RequestBridge/);
});

test('rejects settings scaffolding with the new owner named', async (t) => {
  const { result } = await create(t, 'papyrus', ['--surface', 'settings']);
  assert.notEqual(result.status, 0);
  assert.match(result.stderr, /moved to OSF Settings/);
});

test('refuses a non-empty destination', async (t) => {
  const parent = await mkdtemp(resolve(tmpdir(), 'create-osfui-'));
  const root = resolve(parent, 'project');
  t.after(() => rm(parent, { recursive: true, force: true }));
  await mkdir(root);
  await writeFile(resolve(root, 'keep.txt'), 'keep');
  const result = spawnSync(process.execPath, [CLI, root, '--yes', '--mod-id', 'acme.widgets',
    '--view', 'panel', '--surface', 'menu', '--integration', 'papyrus'], { encoding: 'utf8' });
  assert.notEqual(result.status, 0);
  assert.equal(await readFile(resolve(root, 'keep.txt'), 'utf8'), 'keep');
});
