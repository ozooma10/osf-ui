import assert from 'node:assert/strict';
import { access, mkdir, mkdtemp, readFile, rm, writeFile } from 'node:fs/promises';
import { tmpdir } from 'node:os';
import { resolve } from 'node:path';
import test from 'node:test';
import { createServer } from 'vite';

import { buildProject } from '../src/build.mjs';
import { checkProject } from '../src/check.mjs';
import { loadProject } from '../src/config.mjs';
import { harnessPlugin } from '../src/harness-plugin.mjs';

async function animationFixture(t) {
  const root = await mkdtemp(resolve(tmpdir(), 'osfui-animation-'));
  t.after(() => rm(root, { recursive: true, force: true }));
  const view = resolve(root, 'src/views/osf.animation/browser');
  await mkdir(view, { recursive: true });
  await writeFile(resolve(root, 'osfui.config.ts'), `
    import { defineConfig } from '${new URL('../src/index.mjs', import.meta.url).href}';
    export default defineConfig({
      modId: 'osf.animation',
      outDir: 'build',
      views: [{
        id: 'browser',
        title: 'Animation Browser',
        description: 'Browse animations',
        kind: 'menu',
        width: 1600,
        height: 900,
        pausesGame: false,
        transparent: true,
        targetVersion: '2.0.0',
      }],
    });
  `);
  await writeFile(resolve(root, 'osfui.mock.ts'), `
    export default {};
    export function install(ctx) {
      ctx.registerTools([{
        id: 'ui-host', kind: 'select', label: 'OSF UI host',
        options: [{ value: 'none', label: 'no host info' }], value: 'none',
      }], () => {});
    }
  `);
  await writeFile(resolve(view, 'index.html'),
    '<link rel="stylesheet" href="/shared/osfui.css"><div id="app"></div>' +
    '<script src="/shared/osfui.js"></script>' +
    '<script src="/shared/gamepadnav.js"></script>' +
    '<script type="module" src="./main.ts"></script>');
  await writeFile(resolve(view, 'main.ts'), 'document.querySelector("#app").textContent = "Animation";');
  return root;
}

test('loads, checks, and builds the OSF Animation project shape', async (t) => {
  const root = await animationFixture(t);
  const project = await loadProject(root, 'build');
  assert.equal(project.modId, 'osf.animation');
  assert.equal(project.views[0].qualifiedId, 'osf.animation/browser');
  assert.equal(project.views[0].pausesGame, false);
  assert.match(project.mockPath, /osfui\.mock\.ts$/);
  assert.equal(await checkProject(project), 1);

  await buildProject(project, { quiet: true });
  const output = resolve(root, 'build/SFSE/Plugins/OSFUI/views/osf.animation/browser');
  const manifest = JSON.parse(await readFile(resolve(output, 'manifest.json'), 'utf8'));
  assert.equal(manifest.targetVersion, '2.0.0');
  assert.equal(Object.hasOwn(manifest, 'permissions'), false);
  assert.equal(await access(resolve(output, 'index.html')).then(() => true, () => false), true);
});

test('injects the protocol-aware harness bootstrap and mock loader', async (t) => {
  const project = await loadProject(await animationFixture(t));
  const plugin = harnessPlugin(project, project.views[0]);
  const transformed = plugin.transformIndexHtml.handler(
    '<link rel="stylesheet" href="/shared/osfui.css">' +
    '<script src="/shared/osfui.js"></script>' +
    '<script src="/shared/gamepadnav.js"></script>', {
    path: '/osf.animation/browser/index.html',
  });
  const tags = transformed.tags;
  assert.equal(tags.length, 3);
  assert.match(transformed.html, /href="\/shared\/osfui\.css"/);
  assert.match(transformed.html, /src="\/shared\/osfui\.js"/);
  assert.match(transformed.html, /src="\/shared\/gamepadnav\.js"/);
  assert.match(tags[0].children, /__OSFUI_HARNESS_META__/);
  assert.equal(tags[1].attrs.src, '/__osfui/bootstrap.js');
  assert.equal(tags[2].attrs.src, '/__osfui/mock-loader.js');

  const loader = await readFile(resolve(import.meta.dirname, '../src/browser/mock-loader.js'), 'utf8');
  assert.match(loader, /installMock/);
  assert.match(loader, /loadError/);

  const runtime = await readFile(resolve(import.meta.dirname, '../src/browser/mock-runtime.js'), 'utf8');
  assert.match(runtime, /onEndpoint/);
  assert.match(runtime, /kind: 'ready'/);
  assert.match(runtime, /kind: 'reply'/);
  assert.doesNotMatch(runtime, /papyrus\.(?:send|request)/);

  const shell = await readFile(resolve(import.meta.dirname, '../src/browser/shell.js'), 'utf8');
  for (const kind of ['button', 'toggle', 'cycle', 'select', 'tool-state']) {
    assert.match(shell, new RegExp(kind));
  }
});

test('dev server serves the complete protocol harness module graph', async (t) => {
  const project = await loadProject(await animationFixture(t));
  const server = await createServer({
    root: project.viewsRoot,
    plugins: [harnessPlugin(project, project.views[0])],
    // This test only exercises serving. Disabling the unused watcher also avoids
    // a Node 24/libuv Windows assertion while its temporary directory is removed.
    server: { host: '127.0.0.1', port: 0, open: false, watch: null, fs: { strict: false } },
    logLevel: 'silent',
  });
  await server.listen();
  try {
    const { port } = server.httpServer.address();
    const origin = `http://127.0.0.1:${port}`;
    const page = await fetch(`${origin}/osf.animation/browser/index.html`);
    const html = await page.text();
    assert.equal(page.status, 200);
    assert.match(html, /__osfui\/bootstrap\.js/);
    assert.match(html, /__osfui\/mock-loader\.js/);
    assert.match(html, /href="\/shared\/osfui\.css"/);
    assert.match(html, /src="\/shared\/osfui\.js"/);
    assert.match(html, /src="\/shared\/gamepadnav\.js"/);

    for (const path of ['/shared/osfui.css', '/shared/osfui.js', '/shared/gamepadnav.js']) {
      const response = await fetch(origin + path);
      assert.equal(response.status, 200, `${path} must be served`);
      assert.ok((await response.text()).length > 0, `${path} must not be empty`);
    }

    const expectedImports = new Map([
      ['/__osfui/bootstrap.js', null],
      ['/__osfui/mock-loader.js', './mock-runtime.js'],
      ['/__osfui/mock-runtime.js', './tools-model.js'],
      ['/__osfui/tools-model.js', null],
      ['/__osfui/mock-entry.js', null],
    ]);
    for (const [path, imported] of expectedImports) {
      const response = await fetch(origin + path);
      const source = await response.text();
      assert.equal(response.status, 200, `${path} must be served`);
      if (imported) assert.match(source, new RegExp(imported.replace('.', '\\.')));
    }
  } finally {
    await server.close();
  }
});
