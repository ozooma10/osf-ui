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
import { configuredDeployRoot, deployBuild, deployViews, deploymentRoot, saveLocalModsRoot } from '../src/game.mjs';
import { harnessPlugin } from '../src/harness-plugin.mjs';
import { papyrusImportPaths } from '../src/papyrus-build.mjs';
import { papyrusWarnings } from '../src/papyrus.mjs';
import { writeZip } from '../src/zip.mjs';
import { composeHelper } from '../../../frontend/scripts/compose-helper.mjs';

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

test('toolchain metadata and packaged helper match the runtime API', async () => {
  const constants = await import('../src/constants.mjs');
  const versionHeader = await readFile(
    resolve(import.meta.dirname, '../../../src/Core/Version.h'),
    'utf8',
  );
  assert.equal(
    constants.OSFUI_RELEASE_VERSION,
    /kOsfuiReleaseVersion\s*=\s*"([^"]+)"/.exec(versionHeader)?.[1],
  );
  assert.equal(constants.HOST_VERSION, constants.OSFUI_RELEASE_VERSION);
  assert.equal(
    constants.BRIDGE_VERSION,
    /kBridgeProtocolVersion\s*=\s*"([^"]+)"/.exec(versionHeader)?.[1],
  );
  assert.equal(
    await readFile(resolve(import.meta.dirname, '../assets/osfui.js'), 'utf8'),
    composeHelper(),
  );
});

test('pre-2.0 targetVersion is previewed and built through the frozen helper', async (t) => {
  const root = await projectFixture(t);
  await writeFile(resolve(root, 'osfui.config.ts'), `export default {
    modId: 'acme.widgets',
    views: [{
      id: 'panel',
      entry: 'index.html?mode=compact#inventory',
      targetVersion: '1.5.0',
    }],
  };`);
  const project = await loadProject(root, 'build');
  assert.equal(project.views[0].targetVersion, '1.5.0');
  assert.equal(project.views[0].entry, 'index.html?mode=compact#inventory');
  assert.equal(manifestFor(project.views[0]).targetVersion, '1.5.0');

  const transformed = harnessPlugin(project, project.views[0])
    .transformIndexHtml.handler('', {
      path: '/acme.widgets/panel/index.html?mode=compact',
    });
  const metaScript = transformed.find((tag) =>
    typeof tag.children === 'string' && tag.children.includes('__OSFUI_HARNESS_META__'));
  assert.match(
    metaScript.children,
    /"viewUrl":"\/acme\.widgets\/panel\/index\.html\?mode=compact&osfui-api=1#inventory"/,
  );

  await buildProject(project, { quiet: true });
  const manifest = JSON.parse(await readFile(
    resolve(root, 'dist/SFSE/Plugins/OSFUI/views/acme.widgets/panel/manifest.json'),
    'utf8',
  ));
  assert.equal(manifest.targetVersion, '1.5.0');
  assert.equal(manifest.entry, 'index.html?mode=compact#inventory');
  const zip = resolve(root, 'release/legacy-view.zip');
  await writeZip(project.outDir, zip);
  const archive = await readFile(zip);
  assert.ok(archive.includes(Buffer.from('index.html?mode=compact#inventory')));
});

test('malformed targetVersion fails before dev or build can misrepresent it', async (t) => {
  const root = await projectFixture(t);
  await writeFile(resolve(root, 'osfui.config.ts'), `export default {
    modId: 'acme.widgets',
    views: [{ id: 'panel', targetVersion: '2.x' }],
  };`);
  await assert.rejects(loadProject(root), /targetVersion must be/);
});

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

test('manifestFor covers every manifest.schema.json property', async (t) => {
  const root = await projectFixture(t);
  await writeFile(resolve(root, 'osfui.config.ts'), `export default {
    modId: 'acme.widgets',
    views: [{ id: 'panel', title: 'Panel', targetVersion: '2.0.0' }],
  };`);
  const project = await loadProject(root);
  const manifest = manifestFor(project.views[0]);

  // The class-killing assertion: every non-$ schema property must be a key
  // manifestFor can emit, so the next added property cannot be silently
  // stripped from author configs.
  const schema = JSON.parse(await readFile(
    resolve(import.meta.dirname, '../../../docs/schema/manifest.schema.json'),
    'utf8',
  ));
  const schemaKeys = Object.keys(schema.properties).filter((key) => !key.startsWith('$'));
  const emitted = new Set(Object.keys(manifest));
  for (const key of schemaKeys) {
    assert.ok(emitted.has(key), `manifestFor drops schema property "${key}"`);
  }
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
  assert.equal(manifest.id, undefined);
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

test('check validates generated manifests and drop-in settings schemas', async (t) => {
  const root = await projectFixture(t);
  const settingsRoot = resolve(root, 'mod/SFSE/Plugins/OSFUI/settings');
  await mkdir(settingsRoot, { recursive: true });
  const schemaPath = resolve(settingsRoot, 'acme.widgets.json');
  await writeFile(schemaPath, JSON.stringify({
    id: 'acme.widgets',
    version: 1,
    targetVersion: '2.0.0',
    groups: [{ settings: [{ key: 'enabled', type: 'bool', default: true }] }],
  }));
  assert.equal(await checkProject(await loadProject(root)), 1);

  await writeFile(schemaPath, JSON.stringify({
    id: 'acme.widgets',
    groups: [{ settings: [{ type: 'bool', default: true }] }],
  }));
  await assert.rejects(
    checkProject(await loadProject(root)),
    /settings-schema.*required property 'key'|settings\\acme\.widgets\.json.*required property 'key'/i,
  );

  await writeFile(schemaPath, JSON.stringify({ id: 'somebody.else', groups: [] }));
  await assert.rejects(checkProject(await loadProject(root)), /filename owns id "acme\.widgets"/);
});

test('build refuses to clobber an output directory it did not write', async (t) => {
  const root = await projectFixture(t);
  const project = await loadProject(root, 'build');
  // A mispointed outDir (e.g. '../../build' in a monorepo) may hold every
  // sibling's output; rm -rf there is unrecoverable. Only a directory carrying
  // the osfui-written marker (or an empty one) may be cleaned.
  await mkdir(project.outDir, { recursive: true });
  await writeFile(resolve(project.outDir, 'somebody-elses-output.txt'), 'precious');
  await assert.rejects(buildProject(project, { quiet: true }), /refusing to delete/i);
  assert.equal(
    await readFile(resolve(project.outDir, 'somebody-elses-output.txt'), 'utf8'),
    'precious',
  );
});

test('build cleans its own previous output, and packages omit the marker', async (t) => {
  const root = await projectFixture(t);
  const project = await loadProject(root, 'build');
  await buildProject(project, { quiet: true });
  await buildProject(project, { quiet: true }); // the marker admits the clean
  const zip = await writeZip(project.outDir, resolve(root, 'out/view.zip'));
  const bytes = await readFile(zip);
  assert.ok(!bytes.includes('.osfui-build.json'), 'marker must not ship in packages');
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

test('loads a recordless scripts-only Papyrus configuration', async (t) => {
  const root = await projectFixture(t);
  await writeFile(resolve(root, 'osfui.config.ts'), `export default {
    modId: 'acme.widgets',
    papyrus: { scriptsOnly: true },
    views: [{ id: 'panel' }]
  };`);
  const project = await loadProject(root);
  assert.deepEqual(project.papyrus, { scriptsOnly: true });
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

test('saving the mods root preserves the Papyrus overrides in local.json', async (t) => {
  const root = await projectFixture(t);
  const localPath = resolve(root, '.osfui/local.json');
  await mkdir(resolve(localPath, '..'), { recursive: true });
  // A Papyrus-only override file, exactly what doctorPapyrus tells authors to
  // hand-write; saving the deploy root must not destroy it.
  await writeFile(localPath, JSON.stringify({
    starfieldRoot: 'D:/Games/Starfield',
    papyrusCompiler: 'D:/CK/PapyrusCompiler.exe',
  }, null, 2));
  await saveLocalModsRoot(root, resolve(root, '..', 'MO2', 'mods'));
  const local = JSON.parse(await readFile(localPath, 'utf8'));
  assert.equal(local.starfieldRoot, 'D:/Games/Starfield');
  assert.equal(local.papyrusCompiler, 'D:/CK/PapyrusCompiler.exe');
  assert.equal(local.modsRoot, resolve(root, '..', 'MO2', 'mods'));
});

test('a malformed local.json fails loudly instead of being silently rewritten', async (t) => {
  const root = await projectFixture(t);
  const localPath = resolve(root, '.osfui/local.json');
  await mkdir(resolve(localPath, '..'), { recursive: true });
  await writeFile(localPath, '{ "modsRoot": '); // truncated by a bad merge
  const project = await loadProject(root);
  // Falling through to the not-configured error (or worse, the interactive
  // prompt that rewrites the file) would destroy the author's config.
  await assert.rejects(configuredDeployRoot(project, undefined), /not valid JSON/);
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

test('compatibility checks allow inert URLs but flag remote loads', async (t) => {
  const root = await projectFixture(t);
  const view = resolve(root, 'src/views/acme.widgets/panel');
  // The exact shapes OSF UI's own settings views ship: a URL string constant
  // rendered as an external link (the browser host opens it in the player's browser)
  // and an inline SVG namespace. Neither is network egress; the framework
  // must pass its own gate.
  await writeFile(resolve(view, 'main.js'), [
    "const NEXUS_PAGE_URL = 'https://www.nexusmods.com/starfield/mods/17711';",
    'document.body.innerHTML =',
    '  \'<a href="\' + NEXUS_PAGE_URL + \'" target="_blank">Nexus</a>\' +',
    '  \'<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 8 8"></svg>\';',
    '',
  ].join('\n'));
  assert.equal(await checkProject(await loadProject(root)), 1);
  // A remote resource load is real egress and stays flagged.
  await writeFile(
    resolve(view, 'index.html'),
    '<main><img src="https://tracker.example/p.gif"></main><script type="module" src="./main.js"></script>',
  );
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
  assert.match(bootstrap, /previewInitialized\(\)/);
  assert.match(bootstrap, /kind: 'preview-initialized'/);
  assert.match(shell, /loadMeta/);
  assert.match(shell, /event\.data\.kind === 'preview-initialized'/);
  assert.match(shell, /Preview initialized/);
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
  assert.ok(walked.has('/__osfui/envelope.js'));
  assert.ok(walked.has('/__osfui/pseudo.js'));
  assert.ok(walked.has('/__osfui/tools-model.js'));
  assert.ok(walked.has('/__osfui/traffic-model.js'));
  assert.ok(walked.has('/__osfui/stage-fit.js'));
  // meta.json lists every project view for the shell's switcher.
  const listing = await fetch(`${origin}/__osfui/meta.json`).then((response) => response.json());
  assert.equal(listing.initial, 'acme.widgets/panel');
  assert.equal(listing.views.length, 1);
  assert.equal(listing.views[0].qualifiedId, 'acme.widgets/panel');
  assert.equal(listing.views[0].version, '2.0.0');
  assert.equal(listing.views[0].bridgeVersion, '2.0');
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

test('the dev server refuses /@fs/ escapes and sensitive files', async (t) => {
  const root = await projectFixture(t);
  // A secret OUTSIDE the project — /@fs/ must not serve it.
  const outside = resolve(root, '..', `osfui-secret-${basename(root)}.txt`);
  await writeFile(outside, 'private key material');
  t.after(() => rm(outside, { force: true }));
  // Sensitive files INSIDE the served root — Vite's default deny list
  // (.env, *.pem, .git) must stay active, which `fs.strict: false` disabled.
  await writeFile(resolve(root, 'src/views/.env'), 'TOKEN=hunter2');
  await writeFile(resolve(root, 'src/views/server.pem'), 'BEGIN PRIVATE KEY');
  const project = await loadProject(root);
  const config = await devServerConfig(project, project.views[0], { open: 'false', port: 0 });
  const server = await createServer({ ...config, logLevel: 'silent' });
  await server.listen();
  t.after(() => server.close());
  const { port } = server.httpServer.address();
  const origin = `http://127.0.0.1:${port}`;
  const escapePath = (await realpath(outside)).replaceAll('\\', '/');
  const fsEscape = await fetch(`${origin}/@fs/${escapePath}`);
  assert.equal(fsEscape.status, 403);
  for (const path of ['/.env', '/server.pem']) {
    const denied = await fetch(origin + path);
    assert.notEqual(denied.status, 200, path);
  }
  // The strict config must not break authoring: the view page still serves.
  const page = await fetch(`${origin}/acme.widgets/panel/index.html`);
  assert.equal(page.status, 200);
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

test('the Papyrus import list only names directories that exist', async (t) => {
  const root = await mkdtemp(resolve(await realpath(tmpdir()), 'osfui-papyrus-'));
  t.after(() => rm(root, { recursive: true, force: true, maxRetries: 10, retryDelay: 100 }));
  const sourceRoot = resolve(root, 'mod/Scripts/Source');
  const apiDir = resolve(root, 'tools/papyrus');
  const ckImports = resolve(root, 'ck/Source/Scripts');
  await mkdir(sourceRoot, { recursive: true });
  await mkdir(apiDir, { recursive: true });
  await mkdir(ckImports, { recursive: true });

  // The scaffolded layout: no Scripts/Source/User. Handing the compiler a
  // nonexistent import folder makes it hard-fail ("Cannot use import folder"),
  // so every scaffolded Papyrus project would fail to build.
  const modern = await papyrusImportPaths(sourceRoot, apiDir, ckImports);
  assert.deepEqual(modern, [sourceRoot, apiDir, ckImports]);
  for (const dir of modern) await access(dir);

  // The legacy layout still needs it: `compilerObject` strips the `User\`
  // prefix, so the object name only resolves against the User folder itself.
  const userRoot = resolve(sourceRoot, 'User');
  await mkdir(userRoot, { recursive: true });
  assert.deepEqual(
    await papyrusImportPaths(sourceRoot, apiDir, ckImports),
    [userRoot, sourceRoot, apiDir, ckImports],
  );
});

test('the browser harness only reads meta fields the plugin actually emits', async (t) => {
  // `mock-runtime.js` read `meta.viewId`, which has never existed — the plugin
  // emits `qualifiedId` — so every harness greeting reported an empty view id.
  // Nothing failed loudly, because reading a missing field just yields
  // undefined. Compare the two sides structurally instead.
  const root = await projectFixture(t);
  const project = await loadProject(root);
  const emitted = new Set();
  harnessPlugin(project, project.views[0]).transformIndexHtml.handler('', { path: '/' })
    .filter((tag) => typeof tag.children === 'string' && tag.children.includes('__OSFUI_HARNESS_META__'))
    .forEach((tag) => {
      const json = tag.children.slice(tag.children.indexOf('=') + 1).replace(/;$/, '');
      Object.keys(JSON.parse(json)).forEach((key) => emitted.add(key));
    });
  assert.ok(emitted.has('qualifiedId'), 'the fixture should produce a meta object');

  const browserDir = resolve(import.meta.dirname, '../src/browser');
  const unknown = new Set();
  for (const file of await readdir(browserDir)) {
    if (!file.endsWith('.js')) continue;
    const source = await readFile(resolve(browserDir, file), 'utf8');
    for (const [, key] of source.matchAll(/(?<![\w/'"`])meta\.([A-Za-z_]\w*)/g)) {
      // mockUrl/mockName are conditional on the project having a mock.
      if (!emitted.has(key) && key !== 'mockUrl' && key !== 'mockName') {
        unknown.add(`${file}: meta.${key}`);
      }
    }
  }
  assert.deepEqual([...unknown], []);
});
