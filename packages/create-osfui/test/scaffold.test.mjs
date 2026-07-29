import assert from 'node:assert/strict';
import { mkdtemp, readFile, rm } from 'node:fs/promises';
import { tmpdir } from 'node:os';
import { dirname, resolve } from 'node:path';
import { spawnSync } from 'node:child_process';
import { fileURLToPath } from 'node:url';
import test from 'node:test';

const HERE = dirname(fileURLToPath(import.meta.url));
const CLI = resolve(HERE, '..', 'src', 'cli.mjs');

for (const [surface, integration, backendPath, backendPattern] of [
  ['menu', 'papyrus', 'mod/Scripts/Source/User/AcmeWidgetsOSFUI.psc', /ListenForViewRequests/],
  ['hud', 'native', 'native/src/main.cpp', /RegisterRequest\("acme\.widgets\.getState"/],
  ['menu', 'native', 'native/src/main.cpp', /OSFUI::API::JsonCommand/],
]) {
  test(`creates the ${surface}/${integration} preset`, async (t) => {
    const parent = await mkdtemp(resolve(tmpdir(), 'create-osfui-'));
    const root = resolve(parent, 'project');
    t.after(() => rm(parent, { recursive: true, force: true }));
    const result = spawnSync(process.execPath, [
      CLI,
      root,
      '--yes',
      '--no-install',
      '--mod-id', 'acme.widgets',
      '--view', 'panel',
      '--surface', surface,
      '--integration', integration,
    ], { encoding: 'utf8' });
    assert.equal(result.status, 0, result.stderr);

    const packageJson = JSON.parse(await readFile(resolve(root, 'package.json'), 'utf8'));
    const config = await readFile(resolve(root, 'osfui.config.ts'), 'utf8');
    const source = await readFile(
      resolve(root, 'src/views/acme.widgets/panel/main.ts'),
      'utf8',
    );
    const mock = await readFile(resolve(root, 'osfui.mock.ts'), 'utf8');

    assert.match(packageJson.devDependencies['@osfui/cli'], /^file:/);
    assert.equal(packageJson.dependencies, undefined);
    assert.doesNotMatch(source, /preact/i);
    assert.match(config, new RegExp(`kind: '${surface}'`));
    assert.match(mock, /defineMock/);
    const tsconfig = JSON.parse(await readFile(resolve(root, 'tsconfig.json'), 'utf8'));
    assert.equal(tsconfig.compilerOptions.strict, true);
    // Hand-written .js view files stay a supported authoring path.
    assert.equal(tsconfig.compilerOptions.allowJs, true);

    const sourceMarker = integration === 'native'
      ? 'acme.widgets.getState'
      : 'osfui.papyrus.request';
    assert.match(source, new RegExp(sourceMarker.replaceAll('.', '\\.')));
    assert.match(await readFile(resolve(root, backendPath), 'utf8'), backendPattern);

    if (integration === 'papyrus') {
      // All three concepts (state, action, request) plus the guards that keep
      // the scaffold working after a save load.
      const script = await readFile(resolve(root, backendPath), 'utf8');
      assert.match(script, /OSFUI\.GetVersion\(\) == 0/);
      assert.match(script, /ListenForViewActions/);
      assert.match(script, /OnOSFUIViewAction/);
      assert.match(script, /SetViewInt/);
      assert.match(script, /RejectViewRequest/);
      const alias = await readFile(
        resolve(root, 'mod/Scripts/Source/User/AcmeWidgetsOSFUIPlayerAlias.psc'),
        'utf8',
      );
      assert.match(alias, /Extends ReferenceAlias/);
      assert.match(alias, /Event OnPlayerLoadGame\(\)/);
      assert.match(alias, /owner\.RegisterOSFUI\(\)/);
      assert.match(source, /osfui\?\.data\?\.on/);
      assert.match(source, /osfui\?\.action\?\.\('bump', 1\)/);
      assert.doesNotMatch(source, /ui\.papyrusRequest/);
      assert.match(mock, /ctx\.onCommand/);
      assert.match(mock, /papyrus\.result/);
    }

    if (integration === 'native') {
      assert.equal(packageJson.scripts['build:native'], 'node native/build.mjs');
      const xmake = await readFile(resolve(root, 'xmake.lua'), 'utf8');
      assert.match(xmake, /commonlibsf\.plugin/);
      assert.match(xmake, /set_installdir\("mod"\)/);
      assert.match(xmake, /add_requires\("nlohmann_json"\)/);
      assert.match(xmake, /add_packages\("nlohmann_json"\)/);
      const nativeSource = await readFile(resolve(root, 'native/src/main.cpp'), 'utf8');
      assert.match(nativeSource, /#include "OSFUI_JSON\.h"/);
      assert.match(nativeSource, /OSFUI::API::JsonCommand/);
      assert.match(nativeSource, /OSFUI::API::JsonRequest/);
      assert.match(nativeSource, /SubscribeSettings/);
      assert.match(nativeSource, /SubscribeHotkey/);
      assert.match(source, /osfui\.call<DemoState>/);
      assert.match(source, /acme\.widgets\.increment/);
      assert.match(mock, /ctx\.onCommand/);
      const nativeBuild = await readFile(resolve(root, 'native/build.mjs'), 'utf8');
      assert.match(nativeBuild, /delete env\.XSE_SF_MODS_PATH/);
      assert.match(nativeBuild, /\['build', '-P', projectRoot\]/);
      assert.match(await readFile(resolve(root, 'native/include/OSFUI_API.h'), 'utf8'), /struct IOSFUIBridge/);
      assert.match(await readFile(resolve(root, 'native/include/OSFUI_JSON.h'), 'utf8'), /class JsonClient/);
    }
  });
}

test('accepts the legacy --template typescript flag', async (t) => {
  const parent = await mkdtemp(resolve(tmpdir(), 'create-osfui-'));
  t.after(() => rm(parent, { recursive: true, force: true }));
  const result = spawnSync(process.execPath, [
    CLI,
    resolve(parent, 'project'),
    '--yes',
    '--no-install',
    '--mod-id', 'acme.widgets',
    '--view', 'panel',
    '--template', 'typescript',
    '--surface', 'menu',
    '--integration', 'papyrus',
  ], { encoding: 'utf8' });
  assert.equal(result.status, 0, result.stderr);
});

test('rejects the removed javascript template', async (t) => {
  const parent = await mkdtemp(resolve(tmpdir(), 'create-osfui-'));
  t.after(() => rm(parent, { recursive: true, force: true }));
  const result = spawnSync(process.execPath, [
    CLI,
    resolve(parent, 'project'),
    '--yes',
    '--no-install',
    '--mod-id', 'acme.widgets',
    '--view', 'panel',
    '--template', 'javascript',
    '--surface', 'menu',
    '--integration', 'papyrus',
  ], { encoding: 'utf8' });

  assert.notEqual(result.status, 0);
  assert.match(result.stderr, /--template was removed/);
});

for (const integration of ['settings', 'static']) {
  test(`rejects the removed ${integration} workflow`, async (t) => {
    const parent = await mkdtemp(resolve(tmpdir(), 'create-osfui-'));
    t.after(() => rm(parent, { recursive: true, force: true }));
    const result = spawnSync(process.execPath, [
      CLI,
      resolve(parent, 'project'),
      '--yes',
      '--no-install',
      '--mod-id', 'acme.widgets',
      '--view', 'panel',
      '--surface', 'menu',
      '--integration', integration,
    ], { encoding: 'utf8' });

    assert.notEqual(result.status, 0);
    assert.match(result.stderr, /--integration must be papyrus or native/);
  });
}
