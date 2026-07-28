import assert from 'node:assert/strict';
import { mkdtemp, readFile, rm } from 'node:fs/promises';
import { tmpdir } from 'node:os';
import { dirname, resolve } from 'node:path';
import { spawnSync } from 'node:child_process';
import { fileURLToPath } from 'node:url';
import test from 'node:test';

const HERE = dirname(fileURLToPath(import.meta.url));
const CLI = resolve(HERE, '..', 'src', 'cli.mjs');

for (const [template, surface, integration] of [
  ['preact', 'menu', 'papyrus'],
  ['vanilla', 'hud', 'static'],
]) {
  test(`creates the ${template}/${surface}/${integration} preset`, async (t) => {
    const parent = await mkdtemp(resolve(tmpdir(), 'create-osfui-'));
    const root = resolve(parent, 'project');
    t.after(() => rm(parent, { recursive: true, force: true }));
    const result = spawnSync(process.execPath, [
      CLI,
      root,
      '--yes',
      '--no-install',
      '--mod-id', 'acme.widgets',
      '--view', 'panel',
      '--template', template,
      '--surface', surface,
      '--integration', integration,
    ], { encoding: 'utf8' });
    assert.equal(result.status, 0, result.stderr);
    const config = await readFile(resolve(root, 'osfui.config.ts'), 'utf8');
    const source = await readFile(
      resolve(root, `src/views/acme.widgets/panel/main.${template === 'preact' ? 'tsx' : 'ts'}`),
      'utf8',
    );
    assert.match(config, new RegExp(`kind: '${surface}'`));
    assert.match(source, new RegExp(integration === 'static' ? 'no native bridge' : 'papyrusRequest'));
  });
}
