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
const PAPYRUS_APIS = [
  ['OSFUI.psc', 'OSFUI'],
  ['OSFUI_Settings.psc', 'OSFUI_Settings'],
  ['OSFUI_View.psc', 'OSFUI_View'],
];

async function assertPapyrusApis(root) {
  assert.deepEqual(
    (await readdir(resolve(root, 'tools/papyrus'))).sort(),
    PAPYRUS_APIS.map(([file]) => file).sort(),
    'Papyrus projects get the three split compiler APIs and no aggregate',
  );
  for (const [file, scriptName] of PAPYRUS_APIS) {
    assert.match(
      await readFile(resolve(root, 'tools/papyrus', file), 'utf8'),
      new RegExp(`ScriptName ${scriptName} Native Hidden`),
    );
  }
}

test('stores each supported starter as an authored project tree', async () => {
  for (const [preset, representative] of [
    ['menu-papyrus', 'src/views/__OSFUI_MOD_ID__/__OSFUI_VIEW_ID__/main.js'],
    ['menu-native', 'native/src/main.cpp'],
    ['settings-papyrus', 'build-deploy.ps1'],
  ]) {
    const root = resolve(PROJECT_TEMPLATES, preset);
    const paths = (await readdir(root, { recursive: true }))
      .map((path) => path.replaceAll('\\', '/'));
    assert.ok(paths.includes('_gitignore'), `${preset} owns its gitignore template`);
    assert.ok(paths.includes(representative), `${preset} owns ${representative}`);
    assert.match(
      await readFile(resolve(root, representative), 'utf8'),
      /__OSFUI_[A-Z0-9_]+__/,
      `${representative} exposes explicit scaffold tokens`,
    );
  }
});

for (const [surface, integration, modBackendPath, modBackendPattern] of [
  ['menu', 'papyrus', 'mod/Scripts/Source/AcmeWidgetsOSFUI.psc', /Function Bump\(int total\) Global/],
  ['menu', 'native', 'native/src/main.cpp', /OSFUI::API::JsonSend/],
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
    assert.match(result.stdout, /npm run dev/);

    const packageJson = JSON.parse(await readFile(resolve(root, 'package.json'), 'utf8'));
    const config = await readFile(resolve(root, 'osfui.config.ts'), 'utf8');
    const isPapyrusMenu = integration === 'papyrus';
    const sourceFile = isPapyrusMenu ? 'main.js' : 'main.ts';
    const viewRoot = resolve(root, 'src/views/acme.widgets/panel');
    const source = await readFile(
      resolve(viewRoot, sourceFile),
      'utf8',
    );
    const mock = await readFile(resolve(root, 'osfui.mock.ts'), 'utf8');
    const html = await readFile(resolve(viewRoot, 'index.html'), 'utf8');
    const style = isPapyrusMenu
      ? null
      : await readFile(resolve(viewRoot, 'style.css'), 'utf8');

    assert.match(packageJson.devDependencies['@osfui/cli'], /^file:/);
    assert.equal(packageJson.dependencies, undefined);
    assert.doesNotMatch(source, /preact/i);
    assert.match(config, new RegExp(`kind: '${surface}'`));
    assert.match(config, new RegExp(`targetVersion: '${OSFUI_RELEASE_VERSION.replaceAll('.', '\\.')}`));
    assert.match(config, /description: 'Generated/);
    // Only fields that differ from the CLI defaults are scaffolded.
    assert.doesNotMatch(config, /transparent:|hub:|permissions:/);
    assert.equal(config.match(/\bviews:/g)?.length, 1);
    assert.match(mock, /defineMock/);
    assert.match(mock, /OSF-UI-Starter/);
    // The generated project stays a focused starter, not a second reference.
    await assert.rejects(readFile(resolve(root, 'FEATURES.md'), 'utf8'));
    const readme = await readFile(resolve(root, 'README.md'), 'utf8');
    assert.match(readme, /## (?:API references|Native bridge example)/);
    assert.match(readme, /authoring-settings\.md/);
    assert.doesNotMatch(readme, /FEATURES\.md/);
    if (integration === 'native') {
      assert.match(
        await readFile(resolve(root, 'mod/SFSE/Plugins/OSFUI/l10n/acme.widgets_de.json'), 'utf8'),
        /views\.panel\.heading/,
      );
    } else {
      await assert.rejects(
        readFile(resolve(root, 'mod/SFSE/Plugins/OSFUI/l10n/acme.widgets_de.json'), 'utf8'),
      );
    }
    assert.doesNotMatch(mock, /\btype: 'ui\./);
    const tsconfig = JSON.parse(await readFile(resolve(root, 'tsconfig.json'), 'utf8'));
    assert.equal(tsconfig.compilerOptions.strict, true);
    // Hand-written .js view files stay a supported authoring path.
    assert.equal(tsconfig.compilerOptions.allowJs, true);

    const sourceMarker = integration === 'native'
      ? 'request<DemoState>'
      : 'osfui.papyrus.call';
    assert.match(source, new RegExp(sourceMarker.replaceAll('.', '\\.')));
    assert.match(await readFile(resolve(root, modBackendPath), 'utf8'), modBackendPattern);

    if (integration === 'papyrus') {
      const generatedPaths = await readdir(root, { recursive: true });
      assert.equal(
        generatedPaths.some((path) => /\.(?:esm|esp|esl)$/i.test(path)),
        false,
        'Papyrus presets must not generate a game plugin',
      );
      assert.equal(
        generatedPaths.some((path) => /(?:^|[\\/])spriggit(?:[\\/]|$)/i.test(path)),
        false,
        'Papyrus presets must not generate a Spriggit project',
      );
      assert.equal(packageJson.scripts.setup, undefined);
      assert.equal(packageJson.scripts.package, undefined);
      assert.equal(packageJson.scripts.build, 'npm run build:papyrus && npm run build:view');
      assert.equal(packageJson.scripts['build:papyrus'], 'pwsh -NoProfile -File ./build-papyrus.ps1');
      assert.equal(packageJson.scripts['build:view'], 'osfui build');
      const script = await readFile(resolve(root, modBackendPath), 'utf8');
      assert.match(script, /ScriptName AcmeWidgetsOSFUI Hidden/);
      assert.match(script, /Function Refresh\(\) Global/);
      assert.match(script, /OSFUI_View\.SetState/);
      assert.doesNotMatch(
        script,
        /\bOSFUI\.(?:Get|Set|Reset|Register|Listen|Reply|Reject|Unregister|Open|Close|Send)/,
      );
      assert.doesNotMatch(config, /\bpapyrus\s*:/);
      await assertPapyrusApis(root);
      const papyrusBuild = await readFile(resolve(root, 'build-papyrus.ps1'), 'utf8');
      assert.match(papyrusBuild, /-i=\$sourceRoot;\$osfuiApis;\$PapyrusSource/);
      assert.match(papyrusBuild, /compiler declarations only/);
      assert.match(readme, /current `@osfui\/cli` is a view builder/);
      assert.match(readme, /no ESM,[\s\S]*registration to maintain/);
      assert.doesNotMatch(readme, /Spriggit/i);
      assert.match(readme, /## Build/);
      assert.match(readme, /## Debug/);
      assert.doesNotMatch(readme, /npm run package/);
      assert.equal(readme.match(/npm run dev`/g)?.length, 1);

      // The simplest backend gets a two-file view: static HTML with inline CSS
      // and small browser-ready JavaScript using only the OSF UI API.
      assert.equal(sourceFile, 'main.js');
      await assert.rejects(readFile(resolve(viewRoot, 'main.ts'), 'utf8'));
      await assert.rejects(readFile(resolve(viewRoot, 'style.css'), 'utf8'));
      assert.match(html, /<style>[\s\S]*<\/style>/);
      assert.match(html, /<button id="bump"/);
      assert.match(html, /<p id="status" role="status">/);
      assert.match(html, /<script type="module" src="\.\/main\.js"><\/script>/);
      assert.doesNotMatch(html, /<link|osfui\.css|style\.css/);
      assert.doesNotMatch(source, /import type|<number>|<string>|<\{ args:|:\s*(?:string|unknown|OSFUIHelper)\b/);
      assert.match(source, /^import '\/shared\/osfui\.js';/);
      assert.doesNotMatch(source, /i18n|theme|settings\.changed|ui\.hotkey|handleBack/);
      assert.match(script, /Function Bump\(int total\) Global/);
      assert.match(source, /osfui\.state\.on\('clicks'/);
      assert.match(source, /osfui\.on\('notice', \(\{ args \}\) =>/);
      assert.match(source, /osfui\.papyrus\.call\('AcmeWidgetsOSFUI', 'Bump', clickTotal \+ 1\)/);
      assert.doesNotMatch(script, /ListenForView|RegisterFor/);
      assert.doesNotMatch(script, /Function (?:OpenSettings|Greet)\(/);
      assert.match(readme, /papyrus\.call\(\)/);
      assert.match(readme, /GLOBAL call[\s\S]*intentional escape hatch/);
      assert.doesNotMatch(`${source}\n${script}\n${readme}`, /papyrus\.(?:send|request)/);
      assert.doesNotMatch(source, /ui\.papyrusRequest/);
      assert.match(mock, /name === 'papyrus\.call'/);
      for (const fn of ['Refresh', 'Bump']) {
        assert.match(script, new RegExp(`Function ${fn}\\(`), `.psc implements ${fn}`);
        assert.match(mock, new RegExp(`payload\\.function === '${fn}'`), `mock implements ${fn}`);
      }
      assert.match(script, /OSFUI_View\.SetState\("acme\.widgets", "clicks", total\)/);
      assert.match(mock, /state\.clicks = Number\(args\[0\]\) \|\| 0;/);
      assert.match(mock, /if \(!settingValues\.enabled\)/);
    }

    if (integration === 'native') {
      assert.equal(packageJson.scripts['build:native'], 'node native/build.mjs');
      assert.equal(packageJson.scripts['build:view'], 'osfui build');
      assert.equal(packageJson.scripts.build, 'npm run build:native && npm run build:view');
      assert.equal(packageJson.scripts.package, undefined);
      const xmake = await readFile(resolve(root, 'xmake.lua'), 'utf8');
      assert.match(xmake, /commonlibsf\.plugin/);
      assert.match(xmake, /set_installdir\("mod"\)/);
      assert.match(xmake, /add_requires\("nlohmann_json"\)/);
      assert.match(xmake, /add_packages\("nlohmann_json"\)/);
      const nativeSource = await readFile(resolve(root, 'native/src/main.cpp'), 'utf8');
      assert.match(nativeSource, /#include "OSFUI_JSON\.h"/);
      assert.match(nativeSource, /OSFUI::API::JsonSend/);
      assert.match(nativeSource, /OSFUI::API::JsonRequest/);
      // The frozen C++ ABI remains exact and qualified. MessageBridge gives the
      // owning browser document the short aliases asserted below.
      assert.match(nativeSource, /constexpr const char\* kModId = "acme\.widgets"/);
      assert.match(nativeSource, /SetViewState\(kModId, "state"/);
      assert.match(nativeSource, /RegisterSend\("acme\.widgets\.increment"/);
      for (const endpoint of ['getState', 'greet', 'recalibrate']) {
        assert.match(nativeSource, new RegExp(`RegisterRequest\\("acme\\.widgets\\.${endpoint}"`));
      }
      assert.doesNotMatch(
        nativeSource,
        /Register(?:Send|Request)\("(?:increment|getState|greet|recalibrate)"/,
      );
      assert.match(nativeSource, /SubscribeSettings/);
      assert.match(nativeSource, /SubscribeHotkey/);
      assert.doesNotMatch(nativeSource, /RegisterSettingsSchema/);
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
      assert.match(source, /request<DemoState>\('getState'/);
      assert.match(source, /osfui\.state\.on<DemoState>\('state'/);
      assert.match(source, /osfui\.send\('increment'/);
      assert.match(source, /osfui\.request<Greeting>\('greet'/);
      assert.match(source, /osfui\.on<\{ message: string \}>\('notice'/);
      assert.doesNotMatch(source, /['"]acme\.widgets\.(?:increment|getState|greet|notice)['"]/);
      assert.doesNotMatch(source, /['"]acme\.widgets\/state['"]/);
      assert.doesNotMatch(source, /on\?\.<DemoState>\('acme\.widgets\.state'/);
      assert.doesNotMatch(mock, /\bcommand ===|\breply\(/);
      assert.match(mock, /io\.reject\('invalid-payload'/);
      assert.match(nativeSource, /OnRecalibrate/);
      assert.match(nativeSource, /\.recalibrate"/);
      const nativeBuild = await readFile(resolve(root, 'native/build.mjs'), 'utf8');
      assert.match(nativeBuild, /delete env\.XSE_SF_MODS_PATH/);
      assert.match(nativeBuild, /\['build', '-P', projectRoot\]/);
      assert.match(await readFile(resolve(root, 'native/include/OSFUI_API.h'), 'utf8'), /struct IOSFUIBridge/);
      assert.match(await readFile(resolve(root, 'native/include/OSFUI_JSON.h'), 'utf8'), /class JsonClient/);
      assert.match(readme, /current `@osfui\/cli` does not merge those trees or create an archive/);
      assert.doesNotMatch(readme, /npm run package/);
    }

    assert.doesNotMatch(config, /openOnStart: true/);
    assert.match(config, /pausesGame: false/);
    assert.match(isPapyrusMenu ? html : source, /<button/);
    assert.doesNotMatch(source, /osfui\.markReady\(\)/);
    if (integration === 'native') {
      assert.match(source, /osfui\.i18n\.localize/);
      assert.match(source, /osfui\.theme\.applyAccent/);
      assert.match(source, /registry\.keyboard\?\.labels/);
      assert.match(source, /never store the label/);
      assert.match(mock, /keyboard: \{ layout: 'en-US', labels:/);
      assert.match(source, /osfui\.send\('osfui\.handleBack'/);
    } else {
      assert.doesNotMatch(source, /i18n|theme|settings\.changed|ui\.hotkey|handleBack/);
    }
    // Settings writes, key capture, and platform-service endpoints stay in the
    // existing settings guide instead of expanding either menu starter.
    assert.doesNotMatch(source, /settings\.captureKey|settings\.set'|settings\.reset'/);
    assert.doesNotMatch(source, /'game\.get'|'ping'/);
    assert.doesNotMatch(source, /feature-grid/);
    if (integration === 'papyrus') {
      const schema = JSON.parse(await readFile(
        resolve(root, 'mod/SFSE/Plugins/OSFUI/settings/acme.widgets.json'),
        'utf8',
      ));
      // One group, no pages, no presets: a starting point, not a catalogue.
      assert.equal(schema.groups.length, 1);
      assert.equal(schema.pages, undefined);
      assert.equal(schema.presets, undefined);
      assert.deepEqual(
        schema.groups[0].settings.map(({ key }) => key),
        ['enabled', 'mode', 'intensity', 'greeting', 'accent', 'openKey'],
      );
    }
  });
}

test('escapes punctuation when an opaque mod id becomes generated TypeScript', async (t) => {
  const parent = await mkdtemp(resolve(tmpdir(), 'create-osfui-'));
  const root = resolve(parent, 'project');
  t.after(() => rm(parent, { recursive: true, force: true }));
  const modId = 'Pilot\'s HUD';
  const result = spawnSync(process.execPath, [
    CLI,
    root,
    '--yes',
    '--no-install',
    '--mod-id', modId,
    '--view', 'panel',
    '--surface', 'menu',
    '--integration', 'native',
  ], { encoding: 'utf8' });
  assert.equal(result.status, 0, result.stderr);

  for (const file of [
    resolve(root, 'osfui.config.ts'),
    resolve(root, 'osfui.mock.ts'),
    resolve(root, `src/views/${modId}/panel/main.ts`),
  ]) {
    const source = await readFile(file, 'utf8');
    assert.match(source, /Pilot\\'s HUD/);
    assert.doesNotMatch(source, /'Pilot's HUD/);
  }
});

test('creates the settings/papyrus preset', async (t) => {
  const parent = await mkdtemp(resolve(tmpdir(), 'create-osfui-'));
  const root = resolve(parent, 'project');
  t.after(() => rm(parent, { recursive: true, force: true }));
  // No --integration and no --view: the settings-only starter implies Papyrus and
  // ships no view at all.
  const result = spawnSync(process.execPath, [
    CLI, root, '--yes', '--no-install', '--mod-id', 'acme.widgets', '--surface', 'settings',
  ], { encoding: 'utf8' });
  assert.equal(result.status, 0, result.stderr);
  assert.match(result.stdout, /build-deploy\.ps1/);
  assert.doesNotMatch(result.stdout, /npm run/);

  // None of the view toolchain: this is a drop-in mod folder, not a project.
  for (const absent of ['package.json', 'osfui.config.ts', 'osfui.mock.ts', 'tsconfig.json', 'src']) {
    await assert.rejects(readFile(resolve(root, absent), 'utf8'), absent);
  }

  const schema = JSON.parse(await readFile(
    resolve(root, 'mod/SFSE/Plugins/OSFUI/settings/acme.widgets.json'), 'utf8',
  ));
  assert.equal(schema.id, 'acme.widgets');
  assert.equal(schema.targetVersion, OSFUI_RELEASE_VERSION);
  const rows = schema.groups[0].settings;
  assert.deepEqual(rows.map(({ key }) => key), ['enabled', 'strength', 'mode', 'notifyKey']);
  // Keep the settings-only starter focused on stored values and a hotkey. Authors
  // can add an action row later and target a qualified OSFUI_View request endpoint.
  assert.equal(rows.some((row) => row.type === 'action'), false);

  const script = await readFile(
    resolve(root, 'mod/Scripts/Source/AcmeWidgetsOSFUI.psc'), 'utf8',
  );
  // onPress resolves the target by NAME at delivery time, so the schema and
  // the script must agree exactly or the press silently does nothing.
  const hotkey = rows.find(({ key }) => key === 'notifyKey');
  assert.deepEqual(hotkey.onPress, { script: 'AcmeWidgetsOSFUI', function: 'OnHotkey' });
  assert.equal(hotkey.type, 'key');
  assert.match(script, /^ScriptName AcmeWidgetsOSFUI Hidden/m);
  assert.match(script, /Function OnHotkey\(string asModId, string asKey\) Global/);
  // Reads the toggle, the slider, and the dropdown back out.
  assert.match(script, /OSFUI_Settings\.GetBool\(asModId, "enabled"/);
  assert.match(script, /OSFUI_Settings\.GetInt\(asModId, "strength"/);
  assert.match(script, /OSFUI_Settings\.GetString\(asModId, "mode"/);
  assert.doesNotMatch(script, /\bOSFUI\.(?:Get|Set|Reset|Register|Listen|Unregister)/);
  assert.doesNotMatch(script, /RegisterForHotkey|OnPlayerLoadGame/);

  const build = await readFile(resolve(root, 'build-deploy.ps1'), 'utf8');
  // Without tools/papyrus on the import path the OSFUI_Settings.* calls above will not
  // resolve and the compile fails.
  assert.match(build, /-i=\$sourceRoot;\$osfuiApis;\$PapyrusSource/);
  assert.match(build, /Starfield_Papyrus_Flags\.flg/);
  await assertPapyrusApis(root);
  await assert.rejects(
    readFile(resolve(root, 'mod/SFSE/Plugins/OSFUI/l10n/acme.widgets_de.json'), 'utf8'),
  );

  const readme = await readFile(resolve(root, 'README.md'), 'utf8');
  assert.match(readme, /build-deploy\.ps1/);
  assert.match(readme, /save, reload, and press it again/);
  assert.match(readme, /OSFUI_View\.RegisterRequest/);
  assert.match(readme, /qualified command/);
  assert.doesNotMatch(readme, /npm run/);
});

test('rejects a native settings project it has no way to generate', async (t) => {
  const parent = await mkdtemp(resolve(tmpdir(), 'create-osfui-'));
  t.after(() => rm(parent, { recursive: true, force: true }));
  const result = spawnSync(process.execPath, [
    CLI, resolve(parent, 'project'), '--yes', '--no-install',
    '--mod-id', 'acme.widgets', '--surface', 'settings', '--integration', 'native',
  ], { encoding: 'utf8' });

  assert.notEqual(result.status, 0);
  assert.match(result.stderr, /--surface settings is Papyrus-only/);
});

test('rejects HUD until an authored starter exists', async (t) => {
  const parent = await mkdtemp(resolve(tmpdir(), 'create-osfui-'));
  t.after(() => rm(parent, { recursive: true, force: true }));
  const result = spawnSync(process.execPath, [
    CLI, resolve(parent, 'project'), '--yes', '--no-install',
    '--mod-id', 'acme.widgets', '--view', 'panel',
    '--surface', 'hud', '--integration', 'papyrus',
  ], { encoding: 'utf8' });

  assert.notEqual(result.status, 0);
  assert.match(result.stderr, /does not include a HUD starter/);
});

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
  // "--surfce menu": with a camel-case-anything parser this scaffolded the
  // default Menu starter and exited 0 — the author finds out much later.
  const result = spawnSync(process.execPath, [
    CLI,
    resolve(parent, 'project'),
    '--yes',
    '--no-install',
    '--mod-id', 'acme.widgets',
    '--view', 'panel',
    '--surfce', 'menu',
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
  const sources = await readdir(resolve(root, 'mod/Scripts/Source'));
  assert.ok(sources.length > 0);
  for (const source of sources) {
    assert.match(source, /^[A-Za-z]/, source);
    const script = await readFile(resolve(root, 'mod/Scripts/Source', source), 'utf8');
    assert.match(script, /^ScriptName [A-Za-z]/m, source);
  }
});
