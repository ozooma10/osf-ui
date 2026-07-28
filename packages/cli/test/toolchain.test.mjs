import assert from 'node:assert/strict';
import { mkdir, mkdtemp, readFile, realpath, writeFile } from 'node:fs/promises';
import { tmpdir } from 'node:os';
import { resolve } from 'node:path';
import test from 'node:test';

import preact from '@preact/preset-vite';
import { createServer } from 'vite';

import { buildProject } from '../src/build.mjs';
import { checkProject } from '../src/check.mjs';
import { loadProject, manifestFor } from '../src/config.mjs';
import { harnessPlugin } from '../src/harness-plugin.mjs';
import { writeZip } from '../src/zip.mjs';

async function projectFixture(t) {
  // realpath expands Windows 8.3 short names (NICKLE~1): watching a file under
  // a short-path root trips a libuv fs-event assertion and aborts the process.
  const root = await mkdtemp(resolve(await realpath(tmpdir()), 'osfui-cli-'));
  t.after(async () => {
    const { rm } = await import('node:fs/promises');
    await rm(root, { recursive: true, force: true });
  });
  const view = resolve(root, 'src/views/acme.widgets/panel');
  await mkdir(view, { recursive: true });
  await writeFile(resolve(root, 'osfui.config.ts'), `export default {
    modId: 'acme.widgets',
    views: [{ id: 'panel', title: 'Panel', width: 800, height: 600 }]
  };`);
  await writeFile(resolve(root, 'osfui.mock.json'), '{"state":{}}');
  await writeFile(
    resolve(view, 'index.html'),
    '<main>Hello</main><script type="module" src="./main.ts"></script>',
  );
  await writeFile(
    resolve(view, 'main.ts'),
    "import '/shared/osfui.css';\nimport '/shared/osfui.js';\n",
  );
  return root;
}

test('loads configuration and creates a production manifest', async (t) => {
  const root = await projectFixture(t);
  const project = await loadProject(root);
  assert.equal(project.views[0].qualifiedId, 'acme.widgets/panel');
  assert.deepEqual(manifestFor(project, project.views[0]).permissions, {
    nativeBridge: true,
    filesystem: false,
    network: false,
  });
});

test('checks, builds, and packages a generated-shaped project', async (t) => {
  const root = await projectFixture(t);
  const project = await loadProject(root, 'build');
  assert.equal(await checkProject(project), 1);
  await buildProject(project, { quiet: true });
  const manifest = JSON.parse(await readFile(
    resolve(root, 'dist/SFSE/Plugins/OSFUI/views/acme.widgets/panel/manifest.json'),
    'utf8',
  ));
  assert.equal(manifest.id, 'panel');
  const zip = resolve(root, 'release/view.zip');
  await writeZip(project.outDir, zip);
  assert.equal((await readFile(zip)).subarray(0, 4).toString('hex'), '504b0304');
});

test('compatibility checks flag remote URLs', async (t) => {
  const root = await projectFixture(t);
  await writeFile(resolve(root, 'src/views/acme.widgets/panel/main.js'), 'fetch("https://example.com")');
  await assert.rejects(checkProject(await loadProject(root)), /remote HTTP URL/);
});

test('development server exposes the harness and injects the bridge before view code', async (t) => {
  const root = await projectFixture(t);
  const project = await loadProject(root);
  const server = await createServer({
    root: project.viewsRoot,
    plugins: [harnessPlugin(project, project.views[0]), preact()],
    server: { host: '127.0.0.1', port: 0, open: false, fs: { strict: false } },
    logLevel: 'silent',
  });
  await server.listen();
  t.after(() => server.close());
  const address = server.httpServer.address();
  const origin = `http://127.0.0.1:${address.port}`;
  const harness = await fetch(`${origin}/__osfui/`).then((response) => response.text());
  const view = await fetch(`${origin}/acme.widgets/panel/index.html`).then((response) => response.text());
  const moduleResponse = await fetch(`${origin}/acme.widgets/panel/main.ts`);
  const moduleSource = await moduleResponse.text();
  assert.match(harness, /OSF UI View Harness/);
  assert.match(view, /__osfui\/bootstrap\.js/);
  // The browser JS is served from real files in src/browser/, with the
  // per-view meta prelude prepended to the bootstrap.
  const bootstrap = await fetch(`${origin}/__osfui/bootstrap.js`).then((response) => response.text());
  const shell = await fetch(`${origin}/__osfui/harness.js`).then((response) => response.text());
  assert.match(bootstrap, /^const __OSFUI_HARNESS_META__=\{/);
  assert.match(bootstrap, /osfui-harness/);
  assert.match(shell, /loadMeta/);
  // shell.js is a module importing ./stage-fit.js — the route must exist.
  const stageFit = await fetch(`${origin}/__osfui/stage-fit.js`).then((response) => response.text());
  assert.match(stageFit, /computeFit/);
  assert.equal(moduleResponse.status, 200);
  assert.match(moduleSource, /osfui-shared/);
});
