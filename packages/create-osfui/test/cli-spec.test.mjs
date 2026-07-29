import assert from 'node:assert/strict';
import { mkdir, mkdtemp, rm, writeFile } from 'node:fs/promises';
import { tmpdir } from 'node:os';
import { resolve } from 'node:path';
import test from 'node:test';
import { resolveCliSpec } from '../src/cli-spec.mjs';

test('uses a relative file dependency when the sibling CLI package is available', async (t) => {
  const root = await mkdtemp(resolve(tmpdir(), 'create-osfui-spec-'));
  const project = resolve(root, 'examples', 'my-view');
  const localCli = resolve(root, 'packages', 'cli');
  t.after(() => rm(root, { recursive: true, force: true }));
  await mkdir(project, { recursive: true });
  await mkdir(localCli, { recursive: true });
  await writeFile(resolve(localCli, 'package.json'), '{"name":"@osfui/cli"}\n');

  assert.equal(await resolveCliSpec(project, undefined, localCli), 'file:../../packages/cli');
});

test('uses the registry dependency outside the OSF UI workspace', async (t) => {
  const root = await mkdtemp(resolve(tmpdir(), 'create-osfui-spec-'));
  t.after(() => rm(root, { recursive: true, force: true }));

  assert.equal(await resolveCliSpec(root, undefined, resolve(root, 'missing')), '^0.1.0');
  assert.equal(await resolveCliSpec(root, 'workspace:*', resolve(root, 'missing')), 'workspace:*');
});
