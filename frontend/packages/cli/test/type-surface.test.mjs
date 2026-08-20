import assert from 'node:assert/strict';
import { readFile, readdir } from 'node:fs/promises';
import { resolve } from 'node:path';
import test from 'node:test';

test('baseline project templates contain no TypeScript or frontend build surface', async () => {
  const cliRoot = resolve(import.meta.dirname, '..');
  const repositoryRoot = resolve(cliRoot, '..', '..', '..');
  const templates = resolve(repositoryRoot, 'frontend/packages/create-osfui/templates/projects');
  for (const preset of ['menu-native', 'menu-papyrus']) {
    const paths = (await readdir(resolve(templates, preset), { recursive: true }))
      .map((path) => path.replaceAll('\\', '/'));
    assert.equal(paths.some((path) => /\.(?:ts|tsx|jsx)$/i.test(path)), false, preset);
    for (const absent of ['package.json', 'tsconfig.json', 'osfui.config.js', 'osfui.mock.js']) {
      assert.equal(paths.includes(absent), false, `${preset}/${absent}`);
    }
  }
});

test('public mock types expose onEndpoint and no legacy onCommand', async () => {
  const declarations = await readFile(resolve(import.meta.dirname, '../src/index.d.ts'), 'utf8');
  assert.match(declarations, /onEndpoint\(handler: EndpointHandler\)/);
  assert.doesNotMatch(declarations, /onCommand|papyrus\.(?:send|request)/);
});
