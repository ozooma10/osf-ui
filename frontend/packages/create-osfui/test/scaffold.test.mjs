import assert from 'node:assert/strict';
import { mkdtemp, readFile, readdir, rm } from 'node:fs/promises';
import { tmpdir } from 'node:os';
import { dirname, resolve } from 'node:path';
import { spawnSync } from 'node:child_process';
import { fileURLToPath } from 'node:url';
import test from 'node:test';

import { OSFUI_RELEASE_VERSION } from '@osfui/cli/constants';

const HERE = dirname(fileURLToPath(import.meta.url));
const CLI = resolve(HERE, '..', 'src', 'cli.mjs');
const PROJECT_TEMPLATES = resolve(HERE, '..', 'templates', 'projects');

test('stores each supported starter as an authored project tree', async () => {
  for (const [preset, representative] of [
    ['menu-papyrus', 'src/views/__OSFUI_MOD_ID__/__OSFUI_VIEW_ID__/main.js'],
    ['menu-native', 'native/src/main.cpp'],
    ['hud-papyrus', 'src/views/__OSFUI_MOD_ID__/__OSFUI_VIEW_ID__/main.ts'],
    ['hud-native', 'src/views/__OSFUI_MOD_ID__/__OSFUI_VIEW_ID__/main.ts'],
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
  ['hud', 'papyrus', 'mod/Scripts/Source/AcmeWidgetsOSFUI.psc', /Function Refresh\(\) Global/],
  ['hud', 'native', 'native/src/main.cpp', /UpdateHudState/],
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
    if (integration === 'papyrus') assert.match(result.stdout, /npm run doctor/);

    const packageJson = JSON.parse(await readFile(resolve(root, 'package.json'), 'utf8'));
    const config = await readFile(resolve(root, 'osfui.config.ts'), 'utf8');
    const isPlainPapyrusMenu = surface === 'menu' && integration === 'papyrus';
    const sourceFile = isPlainPapyrusMenu ? 'main.js' : 'main.ts';
    const viewRoot = resolve(root, 'src/views/acme.widgets/panel');
    const source = await readFile(
      resolve(viewRoot, sourceFile),
      'utf8',
    );
    const mock = await readFile(resolve(root, 'osfui.mock.ts'), 'utf8');
    const html = await readFile(resolve(viewRoot, 'index.html'), 'utf8');
    const style = isPlainPapyrusMenu
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
    assert.match(mock, surface === 'menu' ? /OSF-UI-Starter/ : /HUD-Beispiel/);
    // The tour document is gone: the generated project is a starter and the
    // exhaustive reference lives in docs/, linked from the README.
    await assert.rejects(readFile(resolve(root, 'FEATURES.md'), 'utf8'));
    const readme = await readFile(resolve(root, 'README.md'), 'utf8');
    assert.match(readme, /## Where to read more/);
    assert.match(readme, /authoring-settings\.md/);
    assert.doesNotMatch(readme, /FEATURES\.md/);
    assert.match(
      await readFile(resolve(root, 'mod/SFSE/Plugins/OSFUI/l10n/acme.widgets_de.json'), 'utf8'),
      /views\.panel\.heading/,
    );
    assert.doesNotMatch(mock, /\btype: 'ui\./);
    const tsconfig = JSON.parse(await readFile(resolve(root, 'tsconfig.json'), 'utf8'));
    assert.equal(tsconfig.compilerOptions.strict, true);
    // Hand-written .js view files stay a supported authoring path.
    assert.equal(tsconfig.compilerOptions.allowJs, true);

    const sourceMarker = surface === 'hud'
      ? 'osfui/settings'
      : integration === 'native'
        ? 'acme.widgets.getState'
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
      const script = await readFile(resolve(root, modBackendPath), 'utf8');
      assert.match(script, /ScriptName AcmeWidgetsOSFUI Hidden/);
      assert.match(script, /Function Refresh\(\) Global/);
      assert.match(script, /SetViewInt/);
      assert.match(config, /papyrus:\s*\{/);
      assert.match(config, /scriptsOnly: true/);
      assert.doesNotMatch(config, /papyrus: \{ script:/);
      assert.match(
        await readFile(resolve(root, 'tools/papyrus/OSFUI.psc'), 'utf8'),
        /ScriptName OSFUI Native Hidden/,
      );
      assert.match(readme, /npm run doctor/);
      assert.match(readme, /no ESM,[\s\S]*startup quest, alias, or registration/);
      assert.doesNotMatch(readme, /Spriggit/i);
      if (surface === 'menu') {
        assert.match(readme, /## Build/);
        assert.match(readme, /## Debug/);
        assert.match(readme, /Instantiated views reload automatically; press F12 to open DevTools/);
        assert.doesNotMatch(readme, /SpriggitCLI\.zip/);
        assert.doesNotMatch(readme, /Spriggit\.CLI\.exe serialize/);
        assert.equal(readme.match(/npm run dev`/g)?.length, 1);
        assert.equal(readme.match(/npm run package`/g)?.length, 1);
      }

      if (surface === 'menu') {
        // The simplest backend gets a two-file view: static HTML with inline
        // CSS and small browser-ready JavaScript using only the OSF UI API.
        assert.equal(sourceFile, 'main.js');
        await assert.rejects(readFile(resolve(viewRoot, 'main.ts'), 'utf8'));
        await assert.rejects(readFile(resolve(viewRoot, 'style.css'), 'utf8'));
        assert.match(html, /<style>[\s\S]*<\/style>/);
        assert.match(html, /<button id="bump"/);
        assert.match(html, /<script type="module" src="\.\/main\.js"><\/script>/);
        assert.doesNotMatch(html, /<link|osfui\.css|style\.css/);
        assert.doesNotMatch(source, /import type|<number>|<string>|<\{ args:|:\s*(?:string|unknown|OSFUIHelper)\b/);
        assert.match(source, /^import '\/shared\/osfui\.js';/);
        assert.doesNotMatch(source, /i18n|theme|settings\.changed|ui\.hotkey|handleBack/);
        // One button demonstrates the complete JS -> Papyrus -> state path.
        assert.match(script, /Function Bump\(int total\) Global/);
        assert.match(source, /osfui\.state\.on\('acme\.widgets\/clicks'/);
        // A recordless GLOBAL script has nowhere to accumulate, so the VIEW
        // owns the running total and passes it in. Both mod backends then simply
        // publish what they were handed.
        assert.match(source, /osfui\.papyrus\.call\('AcmeWidgetsOSFUI', 'Bump', clickTotal \+ 1\)/);
        assert.doesNotMatch(script, /ListenForView|RegisterFor/);
        // The 1.x endpoint and its reply type are gone, not renamed in place.
        assert.doesNotMatch(source, /papyrus\.request|ui\.papyrusRequest/);
        assert.match(mock, /name === 'papyrus\.call'/);
        // The browser mock mirrors the two functions used by the demo.
        for (const fn of ['Refresh', 'Bump']) {
          assert.match(script, new RegExp(`Function ${fn}\\(`), `.psc implements ${fn}`);
          assert.match(mock, new RegExp(`payload\\.function === '${fn}'`), `mock implements ${fn}`);
        }
        // Bump ASSIGNS the total on both sides (the mock used to accumulate,
        // so the same clicks produced different numbers in game and harness).
        assert.match(script, /OSFUI\.SetViewInt\("acme\.widgets", "clicks", total\)/);
        assert.match(mock, /state\.clicks = Number\(args\[0\]\) \|\| 0;/);
        // Bump honours the `enabled` gate on both sides.
        assert.match(mock, /if \(!settingValues\.enabled\)/);
      } else {
        const schema = JSON.parse(await readFile(
          resolve(root, 'mod/SFSE/Plugins/OSFUI/settings/acme.widgets.json'),
          'utf8',
        ));
        assert.equal(schema.id, 'acme.widgets');
        assert.equal(schema.targetVersion, OSFUI_RELEASE_VERSION);
        assert.deepEqual(
          schema.groups[0].settings.map(({ key }) => key),
          ['hudEnabled', 'toggleHud', 'anchor', 'opacity'],
        );
        assert.match(script, /Function Refresh\(\) Global/);
        assert.match(script, /OSFUI\.SetViewBool\("acme\.widgets", "alert"/);
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
        assert.match(nativeSource, /OSFUI::API::JsonSend/);
        assert.match(nativeSource, /OSFUI::API::JsonRequest/);
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
        assert.match(source, /request<DemoState>\('acme\.widgets\.getState'/);
        assert.match(source, /osfui\.state\.on<DemoState>\('acme\.widgets\/state'/);
        assert.match(source, /acme\.widgets\.increment/);
        assert.doesNotMatch(source, /on\?\.<DemoState>\('acme\.widgets\.state'/);
        assert.doesNotMatch(mock, /\bcommand ===|\breply\(/);
        assert.match(mock, /io\.reject\('invalid-payload'/);
        assert.match(nativeSource, /OnRecalibrate/);
        assert.match(nativeSource, /\.recalibrate"/);
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
      assert.doesNotMatch(config, /order:/);
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
      assert.match(mock, /hud-event/);
      assert.match(mock, /hud-hotkey/);
      assert.match(source, /\.notice'/);
      const modBackend = await readFile(resolve(root, modBackendPath), 'utf8');
      assert.match(modBackend, integration === 'native' ? /PushHudNotice/ : /Function Refresh\(\) Global/);
      assert.doesNotMatch(mock, /osfui\.hello/);
    } else {
      assert.doesNotMatch(config, /openOnStart: true/);
      assert.match(config, /pausesGame: false/);
      assert.match(isPlainPapyrusMenu ? html : source, /<button/);
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
      // Settings writes, key capture, and platform-service endpoints stay in
      // the documentation instead of expanding either menu starter.
      assert.doesNotMatch(source, /settings\.captureKey|settings\.set'|settings\.reset'/);
      assert.doesNotMatch(source, /'game\.get'|'ping'/);
      assert.doesNotMatch(source, /feature-grid/);
      const schema = integration === 'papyrus'
        ? JSON.parse(await readFile(
          resolve(root, 'mod/SFSE/Plugins/OSFUI/settings/acme.widgets.json'),
          'utf8',
        ))
        : null;
      if (schema) {
        // One group, no pages, no presets: a starting point, not a catalogue.
        assert.equal(schema.groups.length, 1);
        assert.equal(schema.pages, undefined);
        assert.equal(schema.presets, undefined);
        assert.deepEqual(
          schema.groups[0].settings.map(({ key }) => key),
          ['enabled', 'mode', 'intensity', 'greeting', 'accent', 'openKey'],
        );
      }
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
  // Papyrus cannot serve an action row's request, so the template must not
  // scaffold one it has no way to answer.
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
  assert.match(script, /OSFUI\.GetBool\(asModId, "enabled"/);
  assert.match(script, /OSFUI\.GetInt\(asModId, "strength"/);
  assert.match(script, /OSFUI\.GetString\(asModId, "mode"/);
  assert.doesNotMatch(script, /RegisterForHotkey|OnPlayerLoadGame/);

  const build = await readFile(resolve(root, 'build-deploy.ps1'), 'utf8');
  // Without tools/papyrus on the import path the OSFUI.* calls above will not
  // resolve and the compile fails.
  assert.match(build, /-i=\$sourceRoot;\$osfuiApi;\$PapyrusSource/);
  assert.match(build, /Starfield_Papyrus_Flags\.flg/);
  assert.match(
    await readFile(resolve(root, 'tools/papyrus/OSFUI.psc'), 'utf8'),
    /ScriptName OSFUI Native Hidden/,
  );
  assert.match(
    await readFile(resolve(root, 'mod/SFSE/Plugins/OSFUI/l10n/acme.widgets_de.json'), 'utf8'),
    /settings\.notifyKey\.label/,
  );

  const readme = await readFile(resolve(root, 'README.md'), 'utf8');
  assert.match(readme, /build-deploy\.ps1/);
  assert.match(readme, /save, reload, and press it again/);
  assert.match(readme, /physical positions/);
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
  // default Menu starter and exited 0 — the author finds out much later.
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
  const sources = await readdir(resolve(root, 'mod/Scripts/Source'));
  assert.ok(sources.length > 0);
  for (const source of sources) {
    assert.match(source, /^[A-Za-z]/, source);
    const script = await readFile(resolve(root, 'mod/Scripts/Source', source), 'utf8');
    assert.match(script, /^ScriptName [A-Za-z]/m, source);
  }
});
