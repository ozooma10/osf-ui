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
  ['hud', 'papyrus', 'mod/Scripts/Source/User/AcmeWidgetsOSFUI.psc', /UpdateHUD/],
  ['hud', 'native', 'native/src/main.cpp', /UpdateHudState/],
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
    if (integration === 'papyrus') assert.match(result.stdout, /npm run doctor/);

    const packageJson = JSON.parse(await readFile(resolve(root, 'package.json'), 'utf8'));
    const config = await readFile(resolve(root, 'osfui.config.ts'), 'utf8');
    const source = await readFile(
      resolve(root, 'src/views/acme.widgets/panel/main.ts'),
      'utf8',
    );
    const mock = await readFile(resolve(root, 'osfui.mock.ts'), 'utf8');
    const style = await readFile(
      resolve(root, 'src/views/acme.widgets/panel/style.css'),
      'utf8',
    );

    assert.match(packageJson.devDependencies['@osfui/cli'], /^file:/);
    assert.equal(packageJson.dependencies, undefined);
    assert.doesNotMatch(source, /preact/i);
    assert.match(config, new RegExp(`kind: '${surface}'`));
    assert.match(config, /targetVersion: '2\.0\.0'/);
    assert.equal(config.match(/\bviews:/g)?.length, 1);
    assert.match(mock, /defineMock/);
    assert.doesNotMatch(mock, /\btype: 'ui\./);
    const tsconfig = JSON.parse(await readFile(resolve(root, 'tsconfig.json'), 'utf8'));
    assert.equal(tsconfig.compilerOptions.strict, true);
    // Hand-written .js view files stay a supported authoring path.
    assert.equal(tsconfig.compilerOptions.allowJs, true);

    const sourceMarker = surface === 'hud'
      ? 'osfui/settings'
      : integration === 'native'
        ? 'acme.widgets.getState'
        : 'osfui.papyrus.request';
    assert.match(source, new RegExp(sourceMarker.replaceAll('.', '\\.')));
    assert.match(await readFile(resolve(root, backendPath), 'utf8'), backendPattern);

    if (integration === 'papyrus') {
      const script = await readFile(resolve(root, backendPath), 'utf8');
      assert.match(script, /OSFUI\.GetVersion\(\) == 0/);
      assert.match(script, /SetViewInt/);
      const alias = await readFile(
        resolve(root, 'mod/Scripts/Source/User/AcmeWidgetsOSFUIPlayerAlias.psc'),
        'utf8',
      );
      assert.match(alias, /Extends ReferenceAlias/);
      assert.match(alias, /Event OnPlayerLoadGame\(\)/);
      assert.match(alias, /owner\.RegisterOSFUI\(\)/);
      assert.match(config, /papyrus:\s*\{/);
      assert.match(config, /plugin: 'AcmeWidgets\.esm'/);
      const pluginHeader = await readFile(
        resolve(root, 'spriggit/AcmeWidgets.esm/RecordData.yaml'),
        'utf8',
      );
      const quest = await readFile(
        resolve(
          root,
          'spriggit/AcmeWidgets.esm/Quests/AcmeWidgetsOSFUIQuest - 000800_AcmeWidgets.esm/RecordData.yaml',
        ),
        'utf8',
      );
      assert.match(pluginHeader, /PackageName: Spriggit\.Yaml\.Starfield/);
      assert.match(pluginHeader, /Version: 0\.35\.1/);
      assert.match(pluginHeader, /Master: Starfield\.esm/);
      assert.match(quest, /StartGameEnabled/);
      assert.match(quest, /Name: AcmeWidgetsOSFUI$/m);
      assert.match(quest, /Name: AcmeWidgetsOSFUIPlayerAlias$/m);
      assert.match(quest, /ForcedReference: 000014:Starfield\.esm/);
      const spriggit = JSON.parse(await readFile(resolve(root, '.spriggit'), 'utf8'));
      assert.deepEqual(spriggit.KnownMasters, [{ ModKey: 'Starfield.esm', Style: 'Full' }]);
      assert.match(
        await readFile(resolve(root, 'tools/papyrus/OSFUI.psc'), 'utf8'),
        /ScriptName OSFUI Native Hidden/,
      );
      const readme = await readFile(resolve(root, 'README.md'), 'utf8');
      assert.match(readme, /npm run doctor/);
      if (surface === 'menu') {
        assert.match(readme, /## Build/);
        assert.match(readme, /## Debug/);
        assert.match(readme, /Loaded views reload automatically; press F12 to open DevTools/);
        assert.doesNotMatch(readme, /SpriggitCLI\.zip/);
        assert.doesNotMatch(readme, /Spriggit\.CLI\.exe serialize/);
        assert.equal(readme.match(/npm run dev`/g)?.length, 1);
        assert.equal(readme.match(/npm run package`/g)?.length, 1);
      }

      if (surface === 'menu') {
        // Interactive menus demonstrate state, one-way actions, and requests.
        assert.match(script, /ListenForViewActions/);
        assert.match(script, /OnOSFUIViewAction/);
        assert.match(script, /RejectViewRequest/);
        assert.match(source, /state\?\.on\?\.<number>\('acme\.widgets\/clicks'/);
        assert.match(source, /state\?\.on\?\.<string>\('acme\.widgets\/greeting'/);
        assert.match(source, /osfui\?\.papyrus\?\.send\('bump', 1\)/);
        // The 1.x endpoint and its reply type are gone, not renamed in place.
        assert.doesNotMatch(source, /ui\.papyrusRequest/);
        assert.doesNotMatch(mock, /papyrus\.result/);
        assert.match(mock, /name === 'papyrus\.request'/);
        assert.match(mock, /io\.resolve/);
      } else {
        const schema = JSON.parse(await readFile(
          resolve(root, 'mod/SFSE/Plugins/OSFUI/settings/acme.widgets.json'),
          'utf8',
        ));
        assert.equal(schema.id, 'acme.widgets');
        assert.equal(schema.targetVersion, '2.0.0');
        assert.deepEqual(
          schema.groups[0].settings.map(({ key }) => key),
          ['hudEnabled', 'toggleHud', 'anchor', 'margin', 'scale', 'opacity', 'accent'],
        );
        assert.match(script, /OSFUI\.OpenMenu\(ViewId\)/);
        assert.match(script, /OSFUI\.SetViewBool\(ModId, "alert"/);
        assert.match(source, /state\?\.on\?\.<string>\('acme\.widgets\/label'/);
        assert.match(source, /state\?\.on\?\.<boolean>\('acme\.widgets\/alert'/);
      }
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
      if (surface === 'menu') {
        assert.match(nativeSource, /OSFUI::API::JsonCommand/);
        assert.match(nativeSource, /OSFUI::API::JsonRequest/);
        assert.match(nativeSource, /SubscribeSettings/);
        assert.match(nativeSource, /SubscribeHotkey/);
        assert.match(source, /request<DemoState>\('acme\.widgets\.getState'/);
        assert.match(source, /state\?\.on\?\.<DemoState>\('acme\.widgets\/state'/);
        assert.match(source, /acme\.widgets\.increment/);
        assert.doesNotMatch(source, /on\?\.<DemoState>\('acme\.widgets\.state'/);
        assert.doesNotMatch(mock, /\bcommand ===|\breply\(/);
        assert.match(mock, /io\.reject\('invalid-payload'/);
      } else {
        assert.match(nativeSource, /RegisterView\(kViewId\)/);
        assert.match(nativeSource, /RegisterSettingsSchema/);
        assert.match(nativeSource, /SetViewState\(kModId, "hud"/);
        assert.doesNotMatch(nativeSource, /SetReadyCallback/);
        assert.match(source, /state\?\.on\?\.<HudState>\('acme\.widgets\/hud'/);
      }
      const nativeBuild = await readFile(resolve(root, 'native/build.mjs'), 'utf8');
      assert.match(nativeBuild, /delete env\.XSE_SF_MODS_PATH/);
      assert.match(nativeBuild, /\['build', '-P', projectRoot\]/);
      assert.match(await readFile(resolve(root, 'native/include/OSFUI_API.h'), 'utf8'), /struct IOSFUIBridge/);
      assert.match(await readFile(resolve(root, 'native/include/OSFUI_JSON.h'), 'utf8'), /class JsonClient/);
    }

    if (surface === 'hud') {
      assert.match(config, /openOnStart: true/);
      assert.match(config, /order: 0/);
      assert.doesNotMatch(source, /<button|<form|addEventListener\('click'/);
      assert.match(source, /settings\.changed/);
      assert.match(source, /ui\.hotkey/);
      // menu.open/menu.close, never a setViewHidden call: the menu policy would
      // clobber a raw hidden flag on the next menu close (see syncVisibility).
      // The prose comment in the template may name the endpoint; a quoted
      // string is a call.
      assert.match(source, /'menu\.open'/);
      assert.doesNotMatch(source, /'setViewHidden'/);
      assert.match(style, /pointer-events: none/);
      assert.match(style, /data-anchor/);
      assert.match(mock, /Change telemetry/);
      assert.match(mock, /hud-hotkey/);
      assert.doesNotMatch(mock, /osfui\.hello/);
    } else {
      assert.doesNotMatch(config, /openOnStart: true/);
      assert.match(source, /<button/);
    }
  });
}

test('rejects --template as unknown (never shipped in a published release)', async (t) => {
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

  assert.notEqual(result.status, 0);
  assert.match(result.stderr, /Unknown option "--template"/);
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

test('rejects an unknown flag instead of silently scaffolding defaults', async (t) => {
  const parent = await mkdtemp(resolve(tmpdir(), 'create-osfui-'));
  t.after(() => rm(parent, { recursive: true, force: true }));
  // "--surfce hud": with a camel-case-anything parser this scaffolded the
  // default menu surface and exited 0 — the author finds out much later.
  const result = spawnSync(process.execPath, [
    CLI,
    resolve(parent, 'project'),
    '--yes',
    '--no-install',
    '--mod-id', 'acme.widgets',
    '--view', 'panel',
    '--surfce', 'hud',
    '--integration', 'native',
  ], { encoding: 'utf8' });

  assert.notEqual(result.status, 0);
  assert.match(result.stderr, /Unknown option "--surfce"/);
});

test('rejects a mod id over the native 64-character cap', async (t) => {
  const parent = await mkdtemp(resolve(tmpdir(), 'create-osfui-'));
  t.after(() => rm(parent, { recursive: true, force: true }));
  const result = spawnSync(process.execPath, [
    CLI,
    resolve(parent, 'project'),
    '--yes',
    '--no-install',
    '--mod-id', `acme.${'w'.repeat(64)}`,
    '--view', 'panel',
    '--surface', 'menu',
    '--integration', 'papyrus',
  ], { encoding: 'utf8' });

  assert.notEqual(result.status, 0);
  assert.match(result.stderr, /64/);
});

test('a digit-leading mod id still yields a legal Papyrus ScriptName', async (t) => {
  const parent = await mkdtemp(resolve(tmpdir(), 'create-osfui-'));
  const root = resolve(parent, 'project');
  t.after(() => rm(parent, { recursive: true, force: true }));
  // "3dscanner.hudpanel" is legal per Ids.h, but a ScriptName and a quest
  // EditorID must start with a letter or the CK compiler refuses the project.
  const result = spawnSync(process.execPath, [
    CLI,
    root,
    '--yes',
    '--no-install',
    '--mod-id', '3dscanner.hudpanel',
    '--view', 'panel',
    '--surface', 'menu',
    '--integration', 'papyrus',
  ], { encoding: 'utf8' });
  assert.equal(result.status, 0, result.stderr);
  const { readdir } = await import('node:fs/promises');
  const sources = await readdir(resolve(root, 'mod/Scripts/Source/User'));
  assert.ok(sources.length > 0);
  for (const source of sources) {
    assert.match(source, /^[A-Za-z]/, source);
    const script = await readFile(resolve(root, 'mod/Scripts/Source/User', source), 'utf8');
    assert.match(script, /^ScriptName [A-Za-z]/m, source);
  }
});
