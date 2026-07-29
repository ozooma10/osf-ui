import assert from 'node:assert/strict';
import { access, mkdir, mkdtemp, readFile, readdir, realpath, rm, stat, utimes, writeFile } from 'node:fs/promises';
import { tmpdir } from 'node:os';
import { basename, resolve } from 'node:path';
import { spawnSync } from 'node:child_process';
import test from 'node:test';

import { createServer } from 'vite';

import { buildProject } from '../src/build.mjs';
import { checkProject } from '../src/check.mjs';
import { loadProject, manifestFor } from '../src/config.mjs';
import { devServerConfig } from '../src/dev.mjs';
import { deployBuild, deployViews, deploymentRoot } from '../src/game.mjs';
import { harnessPlugin } from '../src/harness-plugin.mjs';
import { papyrusWarnings } from '../src/papyrus.mjs';
import { writeZip } from '../src/zip.mjs';

async function projectFixture(t) {
  // realpath expands Windows 8.3 short names (NICKLE~1): watching a file under
  // a short-path root trips a libuv fs-event assertion and aborts the process.
  const root = await mkdtemp(resolve(await realpath(tmpdir()), 'osfui-cli-'));
  t.after(async () => {
    const { rm } = await import('node:fs/promises');
    // Retry: on Windows the dev server's watchers can release the directory a
    // beat after server.close() resolves.
    await rm(root, { recursive: true, force: true, maxRetries: 10, retryDelay: 100 });
  });
  const view = resolve(root, 'src/views/acme.widgets/panel');
  const modAsset = resolve(root, 'mod/Scripts/Example.pex');
  await mkdir(view, { recursive: true });
  await mkdir(resolve(modAsset, '..'), { recursive: true });
  await writeFile(modAsset, 'compiled-papyrus');
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
  assert.deepEqual(manifestFor(project.views[0]).permissions, {
    nativeBridge: true,
    filesystem: false,
    network: false,
  });
});

test('game deployment creates a project-named folder under the MO2 mods directory', async (t) => {
  const root = await projectFixture(t);
  const project = await loadProject(root);
  const modsRoot = resolve(root, '..', 'MO2', 'mods');
  assert.equal(deploymentRoot(project, modsRoot), resolve(modsRoot, basename(root)));
});

test('checks, builds, and packages a generated-shaped project', async (t) => {
  const root = await projectFixture(t);
  const project = await loadProject(root, 'build');
  assert.equal(project.modRoot, resolve(root, 'mod'));
  assert.equal(await checkProject(project), 1);
  await buildProject(project, { quiet: true });
  assert.equal(await readFile(resolve(root, 'dist/Scripts/Example.pex'), 'utf8'), 'compiled-papyrus');
  const manifest = JSON.parse(await readFile(
    resolve(root, 'dist/SFSE/Plugins/OSFUI/views/acme.widgets/panel/manifest.json'),
    'utf8',
  ));
  assert.equal(manifest.id, 'panel');
  const viewsOutput = resolve(root, 'dist/SFSE/Plugins/OSFUI/views');
  assert.equal(
    await access(resolve(viewsOutput, 'shared')).then(() => true, () => false),
    false,
  );
  assert.equal(
    await access(resolve(viewsOutput, 'assets')).then(() => true, () => false),
    false,
  );
  const assets = await readdir(resolve(viewsOutput, 'acme.widgets/assets'));
  assert.ok(assets.some((name) => name.endsWith('.js')));
  assert.ok(assets.some((name) => name.endsWith('.css')));
  const html = await readFile(resolve(viewsOutput, 'acme.widgets/panel/index.html'), 'utf8');
  assert.match(html, /(?:src|href)="\.\.\/assets\//);
  assert.doesNotMatch(html, /(?:src|href)="\.\.\/\.\.\/(?:shared|assets)\//);
  const zip = resolve(root, 'release/view.zip');
  await writeZip(project.outDir, zip);
  const archive = await readFile(zip);
  assert.equal(archive.subarray(0, 4).toString('hex'), '504b0304');
  assert.ok(archive.includes(Buffer.from('Scripts/Example.pex')));
});

test('allows a separate monorepo output directory but rejects overlap with project inputs', async (t) => {
  const root = await projectFixture(t);
  const configPath = resolve(root, 'osfui.config.ts');
  const base = `export default {
    modId: 'acme.widgets',
    views: [{ id: 'panel' }],
    outDir: OUT,
  };`;
  await writeFile(configPath, base.replace('OUT', JSON.stringify('../../build/osfui')));
  assert.equal((await loadProject(root)).outDir, resolve(root, '../../build/osfui'));
  for (const outDir of ['src', '..', 'osfui.config.ts', 'osfui.mock.json']) {
    await writeFile(configPath, base.replace('OUT', JSON.stringify(outDir)));
    await assert.rejects(
      loadProject(root),
      /outDir must be a dedicated directory/,
      outDir,
    );
  }
});

test('game deployment mirrors the completed build so removed mod files do not linger', async (t) => {
  const root = await projectFixture(t);
  const project = await loadProject(root, 'build');
  const deployRoot = resolve(root, 'deployed');
  const deployedAsset = resolve(deployRoot, 'Scripts/Example.pex');
  await buildProject(project, { quiet: true });
  await deployBuild(project, deployRoot);
  assert.equal(await readFile(deployedAsset, 'utf8'), 'compiled-papyrus');
  await rm(resolve(root, 'mod/Scripts/Example.pex'));
  await buildProject(project, { quiet: true });
  await deployBuild(project, deployRoot);
  assert.equal(await access(deployedAsset).then(() => true, () => false), false);
  assert.equal(
    await access(resolve(deployRoot, 'SFSE/Plugins/OSFUI/views/shared/osfui.js'))
      .then(() => true, () => false),
    false,
  );
  assert.equal(
    await access(resolve(deployRoot, 'SFSE/Plugins/OSFUI/views/assets'))
      .then(() => true, () => false),
    false,
  );
  assert.equal(
    await access(resolve(deployRoot, 'SFSE/Plugins/OSFUI/views/acme.widgets/assets'))
      .then(() => true, () => false),
    true,
  );
});

test('hot reload deploys view assets without touching the deployed plugin or scripts', async (t) => {
  const root = await projectFixture(t);
  const project = await loadProject(root, 'build');
  const deployRoot = resolve(root, 'deployed');
  const deployedScript = resolve(deployRoot, 'Scripts/Example.pex');
  await buildProject(project, { quiet: true });
  await deployBuild(project, deployRoot);
  const deployedViewRoot = resolve(
    deployRoot,
    'SFSE/Plugins/OSFUI/views/acme.widgets',
  );
  const originalDirectoryId = (await stat(deployedViewRoot)).ino;
  const originalAssets = await readdir(resolve(deployedViewRoot, 'assets'));
  // Stand in for the game rewriting nothing: a hot reload must leave this
  // file byte-identical, because Starfield holds it open while it runs.
  await writeFile(deployedScript, 'loaded-by-the-game');
  await writeFile(
    resolve(root, 'src/views/acme.widgets/panel/index.html'),
    '<main>Reloaded</main><script type="module" src="./main.ts"></script>',
  );
  await writeFile(
    resolve(root, 'src/views/acme.widgets/panel/main.ts'),
    "import '/shared/osfui.css';\nimport '/shared/osfui.js';\nconsole.log('reloaded');\n",
  );
  await buildProject(project, { quiet: true });
  await deployViews(project, deployRoot);
  assert.equal(await readFile(deployedScript, 'utf8'), 'loaded-by-the-game');
  // Keep the already-enumerated root alive for MO2/USVFS instead of deleting
  // and recreating it. The directory's filesystem identity must not change.
  assert.equal((await stat(deployedViewRoot)).ino, originalDirectoryId);
  assert.match(
    await readFile(resolve(deployRoot, 'SFSE/Plugins/OSFUI/views/acme.widgets/panel/index.html'), 'utf8'),
    /Reloaded/,
  );
  const currentAssets = await readdir(resolve(deployedViewRoot, 'assets'));
  assert.ok(currentAssets.some((name) => !originalAssets.includes(name)));
  assert.ok(originalAssets.some((name) => !currentAssets.includes(name)));
});

test('loads a reproducible Spriggit Papyrus plugin configuration', async (t) => {
  const root = await projectFixture(t);
  const source = resolve(root, 'spriggit/AcmeWidgets.esm');
  await mkdir(source, { recursive: true });
  await writeFile(resolve(source, 'RecordData.yaml'), 'ModKey: AcmeWidgets.esm\n');
  await writeFile(resolve(root, 'osfui.config.ts'), `export default {
    modId: 'acme.widgets',
    papyrus: { plugin: 'AcmeWidgets.esm', source: 'spriggit/AcmeWidgets.esm' },
    views: [{ id: 'panel' }]
  };`);
  const project = await loadProject(root);
  assert.equal(project.papyrus.plugin, 'AcmeWidgets.esm');
  assert.equal(project.papyrus.sourceDir, source);
  assert.equal(project.papyrus.outputPath, resolve(root, 'mod/AcmeWidgets.esm'));
});

test('papyrus check warns about missing and stale compiled scripts, never about extra .pex', async (t) => {
  const root = await projectFixture(t);
  const modRoot = resolve(root, 'mod');
  // The fixture ships Scripts/Example.pex with no source: not a problem.
  assert.deepEqual(await papyrusWarnings(modRoot), []);
  const psc = resolve(modRoot, 'Scripts/Source/User/Acme/Backend.psc');
  await mkdir(resolve(psc, '..'), { recursive: true });
  await writeFile(psc, 'ScriptName Acme:Backend');
  let warnings = await papyrusWarnings(modRoot);
  assert.equal(warnings.length, 1);
  assert.match(warnings[0], /Scripts\/Acme\/Backend\.pex is missing - compile Scripts\/Source\/User\/Acme\/Backend\.psc/);
  const pex = resolve(modRoot, 'Scripts/Acme/Backend.pex');
  await mkdir(resolve(pex, '..'), { recursive: true });
  await writeFile(pex, 'compiled');
  const now = Date.now() / 1000;
  await utimes(psc, now, now);
  await utimes(pex, now, now - 60);
  warnings = await papyrusWarnings(modRoot);
  assert.equal(warnings.length, 1);
  assert.match(warnings[0], /Backend\.pex is older than Scripts\/Source\/User\/Acme\/Backend\.psc/);
  await utimes(pex, now, now + 60);
  assert.deepEqual(await papyrusWarnings(modRoot), []);
  const pluginSource = resolve(root, 'spriggit/AcmeWidgets.esm');
  await mkdir(pluginSource, { recursive: true });
  await writeFile(resolve(pluginSource, 'RecordData.yaml'), 'ModKey: AcmeWidgets.esm\n');
  warnings = await papyrusWarnings(modRoot, {
    plugin: 'AcmeWidgets.esm',
    sourceDir: pluginSource,
    outputPath: resolve(modRoot, 'AcmeWidgets.esm'),
  });
  assert.match(warnings.at(-1), /AcmeWidgets\.esm is missing - run npm run build/);
});

test('package output cannot be placed inside the directory being archived', async (t) => {
  const root = await projectFixture(t);
  const project = await loadProject(root, 'build');
  await buildProject(project, { quiet: true });
  await assert.rejects(
    writeZip(project.outDir, resolve(project.outDir, 'view.zip')),
    /must be outside/,
  );
});

test('CLI rejects unknown options and missing option values', async (t) => {
  const root = await projectFixture(t);
  const cli = resolve(import.meta.dirname, '../src/cli.mjs');
  for (const args of [['doctor', '--wat'], ['dev', '--view', '--game']]) {
    const result = spawnSync(process.execPath, [cli, ...args], {
      cwd: root,
      encoding: 'utf8',
    });
    assert.notEqual(result.status, 0, args.join(' '));
    assert.match(result.stderr, /Unknown option|argument (?:is ambiguous|missing)/);
  }
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
    plugins: [harnessPlugin(project, project.views[0])],
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
  assert.equal(moduleResponse.status, 200);
  assert.match(moduleSource, /osfui-shared/);
  assert.match(harness, /OSF UI View Harness/);
  assert.match(view, /__osfui\/bootstrap\.js/);
  // Injection order is the mock's correctness contract: inline per-view meta,
  // then the classic bootstrap (queuing bridge stub), then the module mock
  // loader, all ahead of the view's own scripts.
  const metaAt = view.indexOf('window.__OSFUI_HARNESS_META__=');
  const bootstrapAt = view.indexOf('/__osfui/bootstrap.js');
  const loaderAt = view.indexOf('/__osfui/mock-loader.js');
  const entryAt = view.indexOf('./main.ts');
  assert.ok(metaAt >= 0 && bootstrapAt >= 0 && loaderAt >= 0 && entryAt >= 0);
  assert.ok(metaAt < bootstrapAt && bootstrapAt < loaderAt && loaderAt < entryAt);
  // The inline meta advertises the mock for this page.
  assert.match(view, /"mockUrl":"\/__osfui\/mock-entry\.js"/);
  // The browser JS is served from real static files in src/browser/.
  const bootstrap = await fetch(`${origin}/__osfui/bootstrap.js`).then((response) => response.text());
  const shell = await fetch(`${origin}/__osfui/harness.js`).then((response) => response.text());
  assert.match(bootstrap, /osfui-harness/);
  assert.match(shell, /loadMeta/);
  // shell.js is a module importing ./stage-fit.js and ./tools-model.js.
  const stageFit = await fetch(`${origin}/__osfui/stage-fit.js`).then((response) => response.text());
  assert.match(stageFit, /computeFit/);
  // Walk the import closure of every /__osfui/ module (shell + mock loader):
  // a src/browser import without a matching served route must fail HERE, not
  // as a silent 404 in the page (which kills the whole mock).
  const pending = ['/__osfui/harness.js', '/__osfui/mock-loader.js'];
  const walked = new Set();
  while (pending.length) {
    const path = pending.pop();
    if (walked.has(path)) continue;
    walked.add(path);
    const response = await fetch(origin + path);
    assert.equal(response.status, 200, `${path} must be served`);
    const source = await response.text();
    for (const [, spec] of source.matchAll(/from\s*['"]([^'"]+)['"]/g)) {
      pending.push(new URL(spec, `http://x${path}`).pathname);
    }
  }
  assert.ok(walked.has('/__osfui/mock-runtime.js'));
  assert.ok(walked.has('/__osfui/pseudo.js'));
  assert.ok(walked.has('/__osfui/tools-model.js'));
  assert.ok(walked.has('/__osfui/traffic-model.js'));
  assert.ok(walked.has('/__osfui/stage-fit.js'));
  // meta.json lists every project view for the shell's switcher.
  const listing = await fetch(`${origin}/__osfui/meta.json`).then((response) => response.json());
  assert.equal(listing.initial, 'acme.widgets/panel');
  assert.equal(listing.views.length, 1);
  assert.equal(listing.views[0].qualifiedId, 'acme.widgets/panel');
  // CSP: locked-down shell, view pages keep the authoring CSP.
  const shellPage = await fetch(`${origin}/__osfui/`);
  assert.match(shellPage.headers.get('content-security-policy'), /script-src 'self'(;|$)/);
  const viewPage = await fetch(`${origin}/acme.widgets/panel/index.html`);
  assert.match(viewPage.headers.get('content-security-policy'), /worker-src 'none'/);
});

test('the mock module is importable through Vite at /__osfui/mock-entry.js', async (t) => {
  const root = await projectFixture(t);
  // A TypeScript mock with a type annotation and an install export — the
  // transform must strip the types and keep both exports.
  const { rm, writeFile: write } = await import('node:fs/promises');
  await rm(resolve(root, 'osfui.mock.json'));
  await write(resolve(root, 'osfui.mock.ts'), [
    "const greeting: string = 'hi';",
    'export default { state: { greeting } };',
    'export function install(ctx: unknown) {}',
  ].join('\n'));
  const project = await loadProject(root);
  const server = await createServer({
    root: project.viewsRoot,
    plugins: [harnessPlugin(project, project.views[0])],
    server: { host: '127.0.0.1', port: 0, open: false, fs: { strict: false } },
    logLevel: 'silent',
  });
  await server.listen();
  t.after(() => server.close());
  const { port } = server.httpServer.address();
  const origin = `http://127.0.0.1:${port}`;
  const entry = await fetch(`${origin}/__osfui/mock-entry.js`);
  const source = await entry.text();
  assert.equal(entry.status, 200);
  assert.doesNotMatch(source, /: string/);
  assert.match(source, /install/);
  assert.match(source, /greeting/);
});

test('the config vite: extension reaches the dev server and stays dev-only', async (t) => {
  const root = await projectFixture(t);
  const { writeFile: write } = await import('node:fs/promises');
  await write(resolve(root, 'osfui.config.ts'), `export default {
    modId: 'acme.widgets',
    views: [{ id: 'panel', title: 'Panel', width: 800, height: 600 }],
    vite: {
      resolve: { alias: { '@dep': '${resolve(root, 'dep').replaceAll('\\', '/')}' } },
      plugins: [{ name: 'project-extra' }],
    },
  };`);
  const { mkdir: makeDir } = await import('node:fs/promises');
  await makeDir(resolve(root, 'dep'), { recursive: true });
  await write(resolve(root, 'dep/answer.ts'), 'export const answer = 42;\n');
  await write(
    resolve(root, 'src/views/acme.widgets/panel/main.ts'),
    "import { answer } from '@dep/answer';\nconsole.log(answer);\n",
  );
  const project = await loadProject(root);
  const config = await devServerConfig(project, project.views[0], { open: 'false', port: 0 });
  // Project plugins land after the harness plugin, which keeps /__osfui/*.
  assert.equal(config.plugins[0].name, 'osfui-author-harness');
  assert.ok(config.plugins.flat().some((plugin) => plugin?.name === 'project-extra'));
  const server = await createServer({ ...config, logLevel: 'silent' });
  await server.listen();
  t.after(() => server.close());
  const { port } = server.httpServer.address();
  const source = await fetch(`http://127.0.0.1:${port}/acme.widgets/panel/main.ts`)
    .then((response) => response.text());
  // The alias resolved: the transformed module references the dep, untouched
  // imports would have 404'd at transform time instead.
  assert.match(source, /answer/);
  assert.doesNotMatch(source, /@dep\/answer/);
  // Build ignores vite: — the project still checks and builds identically.
  const buildProjectShape = await loadProject(root, 'build');
  assert.equal(await checkProject(buildProjectShape), 1);
});

test('a JSON mock flows through the same module entry', async (t) => {
  const root = await projectFixture(t);
  const project = await loadProject(root); // fixture writes osfui.mock.json
  assert.equal(project.mockKind, 'json');
  const server = await createServer({
    root: project.viewsRoot,
    plugins: [harnessPlugin(project, project.views[0])],
    server: { host: '127.0.0.1', port: 0, open: false, fs: { strict: false } },
    logLevel: 'silent',
  });
  await server.listen();
  t.after(() => server.close());
  const { port } = server.httpServer.address();
  const source = await fetch(`http://127.0.0.1:${port}/__osfui/mock-entry.js`)
    .then((response) => response.text());
  // Vite turns JSON into a module with a default export.
  assert.match(source, /export default/);
});
