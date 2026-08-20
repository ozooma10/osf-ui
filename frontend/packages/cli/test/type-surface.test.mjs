import assert from 'node:assert/strict';
import { mkdtemp, readFile, rm, writeFile } from 'node:fs/promises';
import { tmpdir } from 'node:os';
import { relative, resolve } from 'node:path';
import { spawnSync } from 'node:child_process';
import test from 'node:test';

test('generated menu mocks typecheck against the singular MockContext', async (t) => {
  const cliRoot = resolve(import.meta.dirname, '..');
  const repositoryRoot = resolve(cliRoot, '..', '..', '..');
  const templates = resolve(repositoryRoot, 'frontend/packages/create-osfui/templates/projects');
  const temporary = await mkdtemp(resolve(tmpdir(), 'osfui-cli-types-'));
  t.after(() => rm(temporary, { recursive: true, force: true }));
  const configPath = resolve(temporary, 'tsconfig.json');
  const repoFromTemporary = relative(temporary, repositoryRoot).replaceAll('\\', '/');
  await writeFile(configPath, JSON.stringify({
    files: [
      resolve(templates, 'menu-native/osfui.mock.ts'),
      resolve(templates, 'menu-papyrus/osfui.mock.ts'),
    ],
    compilerOptions: {
      noEmit: true,
      strict: true,
      skipLibCheck: true,
      target: 'ES2022',
      module: 'ESNext',
      moduleResolution: 'Bundler',
      paths: {
        '@osfui/cli': [`./${repoFromTemporary}/frontend/packages/cli/src/index.d.ts`],
      },
    },
  }, null, 2));
  const tsc = resolve(repositoryRoot, 'node_modules/typescript/bin/tsc');
  const result = spawnSync(process.execPath, [tsc, '--project', configPath], {
    cwd: repositoryRoot,
    encoding: 'utf8',
  });
  assert.equal(result.status, 0, `${result.stdout}\n${result.stderr}`);
});

test('public mock types expose onEndpoint and no legacy onCommand', async () => {
  const declarations = await readFile(resolve(import.meta.dirname, '../src/index.d.ts'), 'utf8');
  assert.match(declarations, /onEndpoint\(handler: EndpointHandler\)/);
  assert.doesNotMatch(declarations, /onCommand|papyrus\.(?:send|request)/);
});
