import assert from 'node:assert/strict';
import { mkdtemp, readFile, readdir, rm } from 'node:fs/promises';
import { tmpdir } from 'node:os';
import { dirname, resolve } from 'node:path';
import { spawnSync } from 'node:child_process';
import { fileURLToPath } from 'node:url';
import test from 'node:test';

import { OSFUI_RELEASE_VERSION } from '../src/constants.mjs';

const HERE = dirname(fileURLToPath(import.meta.url));
const CLI = resolve(HERE, '..', 'src', 'cli.mjs');
const PROJECT_TEMPLATES = resolve(HERE, '..', 'templates', 'projects');
const REPOSITORY_ROOT = resolve(HERE, '..', '..', '..', '..');
const PAPYRUS_APIS = [
  ['OSFUI.psc', 'OSFUI'],
  ['OSFUI_Settings.psc', 'OSFUI_Settings'],
  ['OSFUI_View.psc', 'OSFUI_View'],
];

const slash = (path) => path.replaceAll('\\', '/');

async function createProject(t, args) {
  const parent = await mkdtemp(resolve(tmpdir(), 'create-osfui-'));
  const root = resolve(parent, 'project');
  t.after(() => rm(parent, { recursive: true, force: true }));
  const result = spawnSync(process.execPath, [CLI, root, '--yes', ...args], { encoding: 'utf8' });
  return { parent, root, result };
}

async function assertMissing(path) {
  await assert.rejects(readFile(path, 'utf8'), { code: 'ENOENT' });
}

async function assertPapyrusApis(root) {
  assert.deepEqual(
    (await readdir(resolve(root, 'tools/papyrus'))).sort(),
    PAPYRUS_APIS.map(([file]) => file).sort(),
  );
  for (const [file, scriptName] of PAPYRUS_APIS) {
    const output = await readFile(resolve(root, 'tools/papyrus', file), 'utf8');
    assert.match(
      output,
      new RegExp(`ScriptName ${scriptName} Native Hidden`),
    );
    assert.equal(
      output,
      await readFile(resolve(REPOSITORY_ROOT, 'data/Scripts/Source', file), 'utf8'),
      `${file} must come from the canonical Papyrus source`,
    );
  }
}

async function assertStaticView(root, modId = 'acme.widgets', viewId = 'panel') {
  const viewRoot = resolve(root, 'mod/SFSE/Plugins/OSFUI/views', modId, viewId);
  assert.deepEqual((await readdir(viewRoot)).sort(), [
    'index.html',
    'main.js',
    'manifest.json',
    'style.css',
  ]);

  const html = await readFile(resolve(viewRoot, 'index.html'), 'utf8');
  const source = await readFile(resolve(viewRoot, 'main.js'), 'utf8');
  const style = await readFile(resolve(viewRoot, 'style.css'), 'utf8');
  const manifest = JSON.parse(await readFile(resolve(viewRoot, 'manifest.json'), 'utf8'));

  assert.equal(manifest.kind, 'menu');
  assert.equal(manifest.entry, 'index.html');
  assert.equal(manifest.pausesGame, false);
  assert.equal(manifest.targetVersion, OSFUI_RELEASE_VERSION);
  assert.match(html, /<link rel="stylesheet" href="\.\/style\.css">/);
  assert.match(html, /<script src="\.\.\/\.\.\/shared\/osfui\.js"><\/script>/);
  assert.match(html, /<script src="\.\/main\.js"><\/script>/);
  assert.doesNotMatch(html, /type="module"|\.tsx?\b/);
  assert.doesNotMatch(source, /^\s*(?:import|export)\b/m);
  assert.doesNotMatch(
    source,
    /osfui\.(?:available|ready|papyrus|i18n|theme)|osfui\.state\.get/,
  );
  assert.match(style, /body\s*\{/);

  const paths = (await readdir(root, { recursive: true })).map(slash);
  for (const forbidden of [
    'package.json',
    'package-lock.json',
    'tsconfig.json',
    'osfui.config.js',
    'osfui.config.ts',
    'osfui.mock.js',
    'osfui.mock.ts',
    'src/vite-env.d.ts',
  ]) {
    assert.equal(paths.includes(forbidden), false, `baseline starter must not emit ${forbidden}`);
  }
  assert.equal(paths.some((path) => /\.(?:ts|tsx|jsx)$/i.test(path)), false);
  assert.equal(paths.some((path) => /(?:^|\/)node_modules(?:\/|$)/.test(path)), false);
  return { viewRoot, html, source, style, manifest };
}

test('stores each supported starter as an authored project tree', async () => {
  for (const obsoleteDirectory of ['native', 'papyrus']) {
    assert.deepEqual(
      await readdir(resolve(PROJECT_TEMPLATES, '..', obsoleteDirectory)).catch((error) => {
        if (error.code === 'ENOENT') return [];
        throw error;
      }),
      [],
      `templates/${obsoleteDirectory} must not contain mirrored SDK files`,
    );
  }

  for (const [preset, representative] of [
    [
      'menu-papyrus',
      'mod/SFSE/Plugins/OSFUI/views/__OSFUI_MOD_ID__/__OSFUI_VIEW_ID__/main.js',
    ],
    ['menu-native', 'native/src/main.cpp'],
    ['settings-papyrus', 'build-deploy.ps1'],
  ]) {
    const root = resolve(PROJECT_TEMPLATES, preset);
    const paths = (await readdir(root, { recursive: true })).map(slash);
    assert.ok(paths.includes('_gitignore'), `${preset} owns its gitignore template`);
    assert.ok(paths.includes(representative), `${preset} owns ${representative}`);
    assert.match(
      await readFile(resolve(root, representative), 'utf8'),
      /__OSFUI_[A-Z0-9_]+__/,
    );
  }
});

test('creates a directly deployable plain-JS Papyrus menu', async (t) => {
  const { root, result } = await createProject(t, [
    '--mod-id', 'acme.widgets',
    '--view', 'panel',
    '--surface', 'menu',
    '--integration', 'papyrus',
  ]);
  assert.equal(result.status, 0, result.stderr);
  assert.match(result.stdout, /build-papyrus\.ps1/);
  assert.doesNotMatch(result.stdout, /npm (?:install|run)/);

  const { source, html } = await assertStaticView(root);
  assert.match(html, /<button id="bump"/);
  assert.match(source, /osfui\.state\.on\("clicks"/);
  assert.match(source, /osfui\.on\("notice"/);
  assert.match(source, /osfui\.send\("papyrus\.call", \{/);
  assert.match(source, /script: "AcmeWidgetsOSFUI"/);
  assert.match(source, /function: "Bump"/);
  assert.doesNotMatch(source, /markReady|papyrus\.(?:send|request)/);

  const script = await readFile(
    resolve(root, 'mod/Scripts/Source/AcmeWidgetsOSFUI.psc'),
    'utf8',
  );
  assert.match(script, /^ScriptName AcmeWidgetsOSFUI Hidden/m);
  assert.match(script, /Function Refresh\(\) Global/);
  assert.match(script, /Function Bump\(int total\) Global/);
  assert.match(script, /Function OpenView\(string asModId, string asKey\) Global/);
  assert.match(script, /OSFUI_View\.Open\(asModId \+ "\/panel"\)/);
  assert.match(script, /OSFUI_View\.SetState\("acme\.widgets", "clicks", total\)/);
  assert.doesNotMatch(script, /ListenForView|RegisterFor/);

  const settings = JSON.parse(await readFile(
    resolve(root, 'mod/SFSE/Plugins/OSFUI/settings/acme.widgets.json'),
    'utf8',
  ));
  const openKey = settings.groups[0].settings.find(({ key }) => key === 'openKey');
  assert.deepEqual(openKey.onPress, {
    script: 'AcmeWidgetsOSFUI',
    function: 'OpenView',
  });
  await assertPapyrusApis(root);

  const build = await readFile(resolve(root, 'build-papyrus.ps1'), 'utf8');
  assert.match(build, /compiler declarations only/);
  assert.match(build, /deploy the complete mod/);
  assert.match(build, /Copy-Item -Path \(Join-Path \$modRoot '\*'\)/);

  const readme = await readFile(resolve(root, 'README.md'), 'utf8');
  assert.match(readme, /nothing to build for the web view/i);
  assert.match(readme, /request\("localName", \.\.\.args\)/);
  assert.match(readme, /Reply` becomes the raw value/);
  assert.doesNotMatch(readme, /npm run|Vite|osfui build/);
});

test('creates a directly deployable plain-JS native menu', async (t) => {
  const { root, result } = await createProject(t, [
    '--mod-id', 'acme.widgets',
    '--view', 'panel',
    '--surface', 'menu',
    '--integration', 'native',
  ]);
  assert.equal(result.status, 0, result.stderr);
  assert.match(result.stdout, /web view is ready to deploy/i);
  assert.doesNotMatch(result.stdout, /npm (?:install|run)/);

  const { source, html } = await assertStaticView(root);
  assert.match(html, /<link rel="stylesheet" href="\.\.\/\.\.\/shared\/osfui\.css">/);
  assert.match(source, /osfui\.state\.on\("state"/);
  assert.match(source, /osfui\.on\("notice"/);
  assert.match(source, /osfui\.send\("increment", \{ amount: 1 \}\)/);
  assert.match(source, /await osfui\.request\("greet", \{ name: name\.value \}\)/);
  assert.doesNotMatch(source, /settings\.changed|ui\.hotkey|i18n|theme/);

  const nativeSource = await readFile(resolve(root, 'native/src/main.cpp'), 'utf8');
  assert.match(nativeSource, /#include "OSFUI_JSON\.h"/);
  assert.match(nativeSource, /OSFUI::API::JsonSend/);
  assert.match(nativeSource, /OSFUI::API::JsonRequest/);
  assert.match(nativeSource, /SetViewState\(kModId, "state"/);
  assert.match(nativeSource, /RegisterSend\("acme\.widgets\.increment"/);
  for (const endpoint of ['getState', 'greet', 'recalibrate']) {
    assert.match(nativeSource, new RegExp(`RegisterRequest\\("acme\\.widgets\\.${endpoint}"`));
  }
  assert.match(nativeSource, /SubscribeSettings/);
  assert.match(nativeSource, /SubscribeHotkey/);

  const schema = JSON.parse(await readFile(
    resolve(root, 'mod/SFSE/Plugins/OSFUI/settings/acme.widgets.json'),
    'utf8',
  ));
  assert.equal(schema.id, 'acme.widgets');
  assert.equal(schema.targetVersion, OSFUI_RELEASE_VERSION);
  assert.deepEqual(
    schema.groups[0].settings.map(({ key }) => key),
    ['enabled', 'mode', 'intensity', 'greeting', 'accent', 'openKey', 'recalibrate'],
  );

  assert.match(await readFile(resolve(root, 'xmake.lua'), 'utf8'), /set_installdir\("mod"\)/);
  await assertMissing(resolve(root, 'native/build.mjs'));
  for (const [file, signature] of [
    ['OSFUI_API.h', /struct IOSFUIBridge/],
    ['OSFUI_JSON.h', /class JsonClient/],
  ]) {
    const output = await readFile(resolve(root, 'native/include', file), 'utf8');
    assert.match(output, signature);
    assert.equal(
      output,
      await readFile(resolve(REPOSITORY_ROOT, 'sdk', file), 'utf8'),
      `${file} must come from the canonical native SDK source`,
    );
  }
  assert.match(
    await readFile(resolve(root, 'mod/SFSE/Plugins/OSFUI/l10n/acme.widgets_de.json'), 'utf8'),
    /views\.panel\.heading/,
  );

  const readme = await readFile(resolve(root, 'README.md'), 'utf8');
  assert.match(readme, /nothing to build for the web view/i);
  assert.match(readme, /owning view calls local endpoints/);
  assert.doesNotMatch(readme, /npm run|Vite|osfui build/);
});

test('keeps opaque mod IDs valid in direct-layout projects', async (t) => {
  const modId = "Pilot's HUD";
  const { root, result } = await createProject(t, [
    '--mod-id', modId,
    '--view', 'panel',
    '--surface', 'menu',
    '--integration', 'native',
  ]);
  assert.equal(result.status, 0, result.stderr);

  const { manifest } = await assertStaticView(root, modId);
  assert.equal(manifest.description, `Generated menu starter for ${modId}`);
  const schema = JSON.parse(await readFile(
    resolve(root, `mod/SFSE/Plugins/OSFUI/settings/${modId}.json`),
    'utf8',
  ));
  assert.equal(schema.id, modId);
  assert.match(await readFile(resolve(root, 'native/src/main.cpp'), 'utf8'), /Pilot's HUD/);
});

test('creates the settings/papyrus preset without frontend files', async (t) => {
  const { root, result } = await createProject(t, [
    '--mod-id', 'acme.widgets',
    '--surface', 'settings',
  ]);
  assert.equal(result.status, 0, result.stderr);
  assert.match(result.stdout, /build-deploy\.ps1/);
  assert.doesNotMatch(result.stdout, /npm run/);

  const paths = (await readdir(root, { recursive: true })).map(slash);
  assert.equal(paths.some((path) => /(?:^|\/)views(?:\/|$)/.test(path)), false);
  assert.equal(paths.includes('package.json'), false);
  assert.equal(paths.some((path) => /\.(?:ts|tsx)$/i.test(path)), false);

  const schema = JSON.parse(await readFile(
    resolve(root, 'mod/SFSE/Plugins/OSFUI/settings/acme.widgets.json'),
    'utf8',
  ));
  assert.equal(schema.id, 'acme.widgets');
  assert.equal(schema.targetVersion, OSFUI_RELEASE_VERSION);
  assert.deepEqual(
    schema.groups[0].settings.map(({ key }) => key),
    ['enabled', 'strength', 'mode', 'notifyKey'],
  );

  const script = await readFile(
    resolve(root, 'mod/Scripts/Source/AcmeWidgetsOSFUI.psc'),
    'utf8',
  );
  assert.match(script, /^ScriptName AcmeWidgetsOSFUI Hidden/m);
  assert.match(script, /Function OnHotkey\(string asModId, string asKey\) Global/);
  await assertPapyrusApis(root);
});

test('rejects a native settings project', async (t) => {
  const { result } = await createProject(t, [
    '--mod-id', 'acme.widgets',
    '--surface', 'settings',
    '--integration', 'native',
  ]);
  assert.notEqual(result.status, 0);
  assert.match(result.stderr, /--surface settings is Papyrus-only/);
});

test('rejects HUD until an authored starter exists', async (t) => {
  const { result } = await createProject(t, [
    '--mod-id', 'acme.widgets',
    '--view', 'panel',
    '--surface', 'hud',
    '--integration', 'papyrus',
  ]);
  assert.notEqual(result.status, 0);
  assert.match(result.stderr, /does not include a HUD starter/);
});

test('--no-install remains a harmless compatibility flag', async (t) => {
  const { root, result } = await createProject(t, [
    '--no-install',
    '--mod-id', 'acme.widgets',
    '--view', 'panel',
    '--surface', 'menu',
    '--integration', 'papyrus',
  ]);
  assert.equal(result.status, 0, result.stderr);
  assert.doesNotMatch(result.stdout, /npm (?:install|run)/);
  await assertStaticView(root);
});

test('rejects obsolete or unknown flags', async (t) => {
  for (const flag of ['--template', '--cli-spec', '--surfce']) {
    const { result } = await createProject(t, [
      '--mod-id', 'acme.widgets',
      '--view', 'panel',
      '--surface', 'menu',
      '--integration', 'papyrus',
      flag,
      'value',
    ]);
    assert.notEqual(result.status, 0, flag);
    assert.match(result.stderr, new RegExp(`Unknown option "${flag}"`));
  }
});

for (const integration of ['settings', 'static']) {
  test(`rejects the removed ${integration} workflow`, async (t) => {
    const { result } = await createProject(t, [
      '--mod-id', 'acme.widgets',
      '--view', 'panel',
      '--surface', 'menu',
      '--integration', integration,
    ]);
    assert.notEqual(result.status, 0);
    assert.match(result.stderr, /--integration must be papyrus or native/);
  });
}

test('rejects a mod ID over the native cap', async (t) => {
  const { result } = await createProject(t, [
    '--mod-id', `acme.${'w'.repeat(64)}`,
    '--view', 'panel',
    '--surface', 'menu',
    '--integration', 'papyrus',
  ]);
  assert.notEqual(result.status, 0);
  assert.match(result.stderr, /64/);
});

test('a digit-leading mod ID still yields a legal Papyrus ScriptName', async (t) => {
  const { root, result } = await createProject(t, [
    '--mod-id', '3dscanner.hudpanel',
    '--view', 'panel',
    '--surface', 'menu',
    '--integration', 'papyrus',
  ]);
  assert.equal(result.status, 0, result.stderr);
  const sources = await readdir(resolve(root, 'mod/Scripts/Source'));
  for (const source of sources) {
    assert.match(source, /^[A-Za-z]/);
    assert.match(await readFile(resolve(root, 'mod/Scripts/Source', source), 'utf8'), /^ScriptName [A-Za-z]/m);
  }
});
