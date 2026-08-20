import assert from 'node:assert/strict';
import { access, mkdir, mkdtemp, readFile, rm, writeFile } from 'node:fs/promises';
import { tmpdir } from 'node:os';
import { resolve } from 'node:path';
import test from 'node:test';

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
        permissions: { nativeBridge: true },
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
    '<div id="app"></div><script src="../../shared/osfui.js"></script><script type="module" src="./main.ts"></script>');
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
  assert.equal(manifest.permissions.nativeBridge, true);
  assert.equal(await access(resolve(output, 'index.html')).then(() => true, () => false), true);
});

test('injects only the harness marker and Animation tool loader', async (t) => {
  const project = await loadProject(await animationFixture(t));
  const plugin = harnessPlugin(project, project.views[0]);
  const tags = plugin.transformIndexHtml.handler('', {
    path: '/osf.animation/browser/index.html',
  });
  assert.equal(tags.length, 3);
  assert.match(tags[0].children, /__OSFUI_HARNESS_META__/);
  assert.equal(tags[1].attrs.src, '/__osfui/bootstrap.js');
  assert.equal(tags[2].attrs.src, '/__osfui/mock-loader.js');

  const loader = await readFile(resolve(import.meta.dirname, '../src/browser/mock-loader.js'), 'utf8');
  assert.match(loader, /registerTools/);
  assert.doesNotMatch(loader, /scenario|onEndpoint|pseudo|traffic/i);
});
