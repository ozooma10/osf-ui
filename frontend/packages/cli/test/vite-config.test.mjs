import assert from 'node:assert/strict';
import { mkdir, mkdtemp, readFile, readdir, rm, writeFile } from 'node:fs/promises';
import { tmpdir } from 'node:os';
import { resolve } from 'node:path';
import test from 'node:test';
import { createServer } from 'vite';

import { buildProject } from '../src/build.mjs';
import { loadProject } from '../src/config.mjs';
import { devServerConfig } from '../src/dev.mjs';

async function fixture(t) {
  const root = await mkdtemp(resolve(tmpdir(), 'osfui-vite-config-'));
  t.after(() => rm(root, { recursive: true, force: true }));
  const view = resolve(root, 'src/views/example.mod/browser');
  await mkdir(view, { recursive: true });
  await writeFile(resolve(root, 'osfui.config.ts'), `
    import { defineConfig } from '${new URL('../src/index.mjs', import.meta.url).href}';
    export default defineConfig({
      modId: 'example.mod',
      outDir: 'output',
      vite: async ({ command, mode }) => ({
        root: 'must-not-win',
        base: '/must-not-win/',
        build: { outDir: 'must-not-win' },
        plugins: [{
          name: 'fixture-vite-config',
          transform(code, id) {
            if (id.replaceAll('\\\\', '/').endsWith('/main.ts')) {
              return code.replace('UNTRANSFORMED', command + ':' + mode);
            }
          },
        }],
      }),
      view: { id: 'browser' },
    });
  `);
  await writeFile(resolve(view, 'index.html'),
    '<div id="app"></div><script type="module" src="./main.ts"></script>');
  await writeFile(resolve(view, 'main.ts'),
    'document.querySelector("#app").textContent = "UNTRANSFORMED";');
  return root;
}

async function javascriptUnder(root) {
  const result = [];
  for (const entry of await readdir(root, { withFileTypes: true })) {
    const path = resolve(root, entry.name);
    if (entry.isDirectory()) result.push(...await javascriptUnder(path));
    else if (entry.name.endsWith('.js')) result.push(path);
  }
  return result;
}

test('applies project Vite plugins without surrendering production layout', async (t) => {
  const root = await fixture(t);
  const project = await loadProject(root, 'build');
  await buildProject(project, { quiet: true });

  const output = resolve(root, 'output/Data/SFSE/Plugins/OSF/UI/views/example.mod');
  const scripts = await javascriptUnder(output);
  assert.ok(scripts.length > 0);
  assert.match(await readFile(scripts[0], 'utf8'), /build:production/);
  assert.equal(await readFile(resolve(output, 'browser/manifest.json'), 'utf8')
    .then(() => true, () => false), true);
  assert.equal(await readFile(resolve(root, 'must-not-win/index.html'), 'utf8')
    .then(() => true, () => false), false);
});

test('applies project Vite plugins inside the development harness', async (t) => {
  const root = await fixture(t);
  const project = await loadProject(root, 'serve');
  const config = await devServerConfig(project, project.views[0], { port: '0', open: 'false' });
  const server = await createServer({
    ...config,
    logLevel: 'silent',
    server: { ...config.server, watch: null },
  });
  await server.listen();
  try {
    const { port } = server.httpServer.address();
    const response = await fetch(`http://127.0.0.1:${port}/example.mod/browser/main.ts`);
    const body = await response.text();
    assert.equal(response.status, 200, body);
    assert.match(body, /serve:development/);
    const harnessModule = await server.transformRequest('/__osfui/mock-loader.js');
    assert.match(harnessModule.code, /installMock/);
  } finally {
    await server.close();
  }
});
