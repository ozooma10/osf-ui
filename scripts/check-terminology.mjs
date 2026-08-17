// Guard the small set of domain-language mistakes that have previously made
// current documentation describe retired protocol or lifecycle behavior.
// Compatibility adapters, migration guides, tests of legacy behavior, and the
// changelog intentionally keep their historical spellings and are not scanned.
import { existsSync, readFileSync, readdirSync } from 'node:fs';
import { dirname, extname, relative, resolve } from 'node:path';
import { fileURLToPath } from 'node:url';

const root = resolve(dirname(fileURLToPath(import.meta.url)), '..');
const failures = [];

function checkText(file, patterns) {
  const absolute = resolve(root, file);
  const text = readFileSync(absolute, 'utf8');
  for (const [pattern, explanation] of patterns) {
    for (const match of text.matchAll(new RegExp(pattern.source, pattern.flags.includes('g') ? pattern.flags : `${pattern.flags}g`))) {
      const line = text.slice(0, match.index).split('\n').length;
      failures.push({ file, line, found: match[0], explanation });
    }
  }
}

function sourceFiles(directory) {
  const files = [];
  for (const entry of readdirSync(resolve(root, directory), { withFileTypes: true })) {
    const path = `${directory}/${entry.name}`;
    if (entry.isDirectory()) {
      if (path.replaceAll('\\', '/').toLowerCase().endsWith('/compat')) continue;
      files.push(...sourceFiles(path));
    } else if (['.cpp', '.css', '.h', '.js', '.ts', '.tsx'].includes(extname(entry.name))) {
      files.push(path);
    }
  }
  return files;
}

const retiredBridgeNames = [
  [/\b(?:settings|views|i18n)\.data\b/, 'use the qualified 2.0 state key'],
  [/\bruntime\.ready\b/, 'call this the bridge ready handshake'],
  [/\bhandoff\.state\b/, 'the first-load handoff protocol was removed'],
  [/\bosfui\/handoff\b/, 'the first-load handoff state was removed'],
  [/\bosfui\.handoffRetry\b/, 'the first-load handoff retry endpoint was removed'],
  [/\bview\.ready\b/, 'main-frame load now gates a pending first open'],
  [/\breadySignal\b/, 'the manifest readiness extension was removed'],
  [/\bmarkReady\b/, 'the shared helper readiness extension was removed'],
];

// These names described older implementations rather than stable public
// compatibility spellings. Keep current source and native tests on the
// vocabulary in docs/terminology.md.
const retiredInternalNames = [
  [/\bMenuController\b/, 'use ViewPresentationController'],
  [/\bViewStateStore\b/, 'use RetainedStateStore'],
  [/\bSurfaceKind\b/, 'use ViewKind'],
  [/\bOnProtocolMisuse\b/, 'use OnProtocolFault'],
  [/\bSetSurfaceLoaded\b/, 'use SetViewInstantiated'],
  [/\bVanillaKey\b/, 'use GameBinding'],
  [/\bResolveInputContext\b/, 'use ResolveHotkeyContext'],
	[/\bRendererHostRecovery\b/, 'use BrowserHostRecovery'],
  [/\bOnHostRestart\b/, 'qualify the browser-host restart'],
	[/\bCommandCoalesceKey\b/, 'use GameMessageCoalesceKey for private browser-host IPC'],
	[/\b(?:Drain|Handle)Commands\b/, 'use game-message terminology for private browser-host IPC'],
	[/\bSetActiveView\b/, 'use SetInputTargetView for the renderer input target'],
	[/\bSetActiveMsg\b/, 'use SetInputTargetMsg; setActive is only the compatibility wire spelling'],
	[/\bHandleSetActive\b/, 'use HandleSetInputTarget; setActive is only the compatibility wire spelling'],
  [/\bFindActiveWidget\b/, 'use FindInputTargetWidget for the browser input target'],
  [/\bLiveViewsOfMod\b/, 'use InstantiatedViewsOfMod for document-instance delivery targets'],
  [/\bRegisterPlatformCommands\b/, 'use RegisterPlatformEndpoints for current sends and requests'],
  [/\bIsValidPluginCommand\b/, 'use IsValidPluginEndpointName for current sends and requests'],
  [/\bruntime[- ]registered\b/i, 'use native-registered for dynamic native API registration'],
  [/\bruntime registration\b/i, 'use native registration for dynamic native API registration'],
  [/manifest id grammar/i, 'use qualified view id grammar'],
  [/\bmanifest id\b/i, 'identity comes from the qualified view path, not the manifest'],
  [/views\/<id>\//, 'use views/<modId>/<viewName>/ or the mod namespace views/<modId>/'],
  [/\bloaded views?\b/i, 'qualify main-frame load state or use instantiated view'],
  [/\bview not loaded\b/i, 'use view not instantiated when no browser object exists'],
  [/\bnever[- ]loaded view\b/i, 'use never-instantiated view'],
  [/\bnot-yet-loaded\b/i, 'use not-yet-instantiated for browser-object lifecycle'],
  [/\btop menu\b/i, 'use active menu; OSF UI has no menu stack'],
  [/LiveControlMap: ready/, 'qualify the game-binding catalog readiness milestone'],
  [/\bViewManager::LoadAll\b/, 'use ViewManager::DiscoverAll for manifest discovery'],
  [/\bLoadView\(/, 'use CreateOrNavigateView for renderer creation/navigation'],
  [/\bIsTargetNewerThanHost\b/, 'use IsTargetNewerThanInstalledRelease'],
  [/\bOnHost(?:Key|Mouse\w*)\b/, 'use game-window input naming'],
  [/\bhostVerdict\b/, 'use bridgeVerdict for MessageBridge validation'],
  [/\bvanillaWarnings\b/, 'use gameBindingWarnings'],
  [/\bmcm-design(?:\.md)?\b/i, 'reference current Mod Settings documentation'],
  [/\bMCM\b/, 'use Mod Settings in current source and tests'],
  [/\bhost-side\b/i, 'qualify browser host or say native desktop/runtime side'],
  [/\bvanilla key(?:s| bindings?| conflicts?)?\b/i, 'use game input action or game binding'],
  [/\bpinned (?:rail|destination|entry)\b/i, 'use fixed for UI placement; pinned is a residency policy'],
];

const retiredFrontendInternalNames = [
  [/\bruntimeVersion\b/, 'qualify the OSF UI release as osfuiReleaseVersion'],
  [/\bsendCommand\b/, 'use sendEndpoint for a one-way endpoint call'],
];

const retiredInternalPaths = [
  'src/Views/MenuController.h',
  'src/Views/MenuController.cpp',
  'src/Views/ViewStateStore.h',
  'src/Views/ViewStateStore.cpp',
	'src/Render/RendererHostRecovery.h',
  'frontend/src/lib/settings/inputContext.ts',
  'frontend/devmock/fixtures/vanillaKeys.ts',
  'packages/create-osfui/src/backend-templates.mjs',
	'tests/native/renderer_host_recovery_tests.cpp',
	'tools/webview2_host/HostCommands.inl',
];

for (const file of sourceFiles('frontend/src').filter((file) => !file.toLowerCase().includes('/compat/'))) {
  checkText(file, retiredBridgeNames);
  checkText(file, retiredInternalNames);
  checkText(file, retiredFrontendInternalNames);
}
for (const file of sourceFiles('src').filter((file) => !file.toLowerCase().includes('/compat/'))) {
  checkText(file, retiredBridgeNames);
  checkText(file, retiredInternalNames);
}
for (const file of sourceFiles('tests/native')) {
  checkText(file, retiredInternalNames);
}
checkText('tests/native/view_presentation_controller_tests.cpp', [
  [/\bmc\b/, 'use controller; mc is a leftover MenuController abbreviation'],
  [/\bViewRegistration\b/, 'use InstantiatedView for presentation-controller records'],
  [/\b(?:Register|Unregister|IsRegistered)\s*\(/, 'use explicit instantiated-view operations'],
]);
for (const file of [
  'src/Views/ViewPresentationController.h',
  'src/Views/ViewPresentationController.cpp',
]) {
  checkText(file, [
    [/\bViewRegistration\b/, 'use InstantiatedView for presentation-controller records'],
    [/\b(?:Register|Unregister|IsRegistered)\s*\(/, 'use explicit instantiated-view operations'],
  ]);
}
checkText('src/Runtime/Runtime.cpp', [
  [/_presentation\.(?:Register|Unregister|IsRegistered)\s*\(/,
    'use explicit instantiated-view presentation operations'],
]);
checkText('frontend/test/settings.discovered-views.test.tsx', [
  [/live loaded\s*->\s*unloaded/i, 'qualify loadState values or use instantiated -> reclaimed'],
]);

for (const file of retiredInternalPaths) {
  if (existsSync(resolve(root, file))) {
    failures.push({ file, line: 1, found: file, explanation: 'use the canonical replacement path' });
  }
}
checkText('sdk/osfui.d.ts', retiredBridgeNames);
checkText('sdk/osfui.d.ts', [
  [/\bloaded views?\b/i, 'qualify main-frame load state or use instantiated view'],
  [/web -> mod backend|MOD BACKEND missed|Named mod-backend-owned values/i,
    'platform and mod endpoints share this API; use endpoint-handler or named-state language'],
]);
checkText('tests/native/README.md', retiredBridgeNames);
checkText('tests/native/README.md', retiredInternalNames);

checkText('frontend/README.md', [
  [/always `type: "ui\.command"`/, 'document the strict 2.0 kind/name/id envelope'],
  [/manifest\.json`'s `id` \*\*must\*\* equal/, 'view identity comes from the modId/viewName path'],
]);
checkText('frontend/test/protocol.envelope.test.ts', [
  [/\bhostVerdict\b/, 'use bridgeVerdict for MessageBridge validation'],
]);
checkText('frontend/test/settings.handshake.test.tsx', [
  [/out-of-process WebView2 backend/, 'use browser-host rendering path'],
  [/\bthe host that carries it\b/i, 'qualify the browser host'],
]);
checkText('frontend/osfui.config.ts', [
  [/\bsurface `osfui dev`/, 'use view'],
]);
checkText('frontend/osfui.mock.ts', [
  [/\bsimulated backend\b/, 'use simulated mod backend'],
]);
checkText('frontend/devmock/mockbridge.ts', [
  [/\bkPluginVersion\b/, 'parse kOsfuiReleaseVersion from Version.h'],
  [/\bpluginVersion\b/, 'use osfuiReleaseVersion for the bridge ready payload'],
]);
for (const file of [
  'frontend/test/keybinds.conflicts.test.ts',
  'frontend/test/keybinds.model.test.ts',
  'frontend/test/keybinds.navigation.test.tsx',
]) {
  checkText(file, [
    [/\bVanillaContextClassification\b/, 'use GameInputContextClassification outside compatibility tests'],
    [/\bvanillaWarnings\b/, 'use gameBindingWarnings'],
    [/\bvanilla\b/i, 'use game binding or Starfield binding'],
  ]);
}
checkText('docs/authoring-views.md', [
  [/open menus stack in open order/, 'OSF UI has one active-menu slot'],
]);
checkText('docs/authoring-settings.md', [
  [/views\/<id>\//, 'use the owning mod namespace views/<modId>/'],
]);
checkText('docs/schema/settings-schema.schema.json', [
  [/views\/<id>\//, 'use the owning mod namespace views/<modId>/'],
]);
checkText('docs/architecture.md', [
  [/Null backends remain selectable/, 'WebView2 and D3D12 are the only production backends'],
]);
checkText('docs/mod-api-2.0-design.md', [
  [/\bhost-side\b/i, 'qualify the OSF UI runtime or browser host'],
  [/osfui\/debug\.error/, 'use the shipped dotted event name osfui.debug.error'],
]);
checkText('docs/troubleshooting.md', [
  [/MOD MENUS/, 'use the MOD SETTINGS pause-menu label'],
]);
checkText('src/Views/ViewManifest.h', [
  [/manifest's `id` field must equal/, 'manifests do not declare identity'],
]);
checkText('src/Views/ViewManager.h', [
  [/\bLoadAll\(/, 'use DiscoverAll for manifest discovery'],
]);
checkText('src/Render/WebView2HostWebRenderer.cpp', [
  [/\bactiveId\b/, 'use inputTargetId for the renderer input target'],
  [/\bactive view\b/i, 'use input-target view in renderer transport language'],
]);
for (const file of [
  'tools/webview2_host/HostApp.cpp',
  'tools/webview2_host/GameMessages.inl',
  'tools/webview2_host/HostGraphics.inl',
]) {
  checkText(file, [
    [/\bactive view\b/i, 'use input-target view in browser-host transport language'],
  ]);
}
checkText('src/Bindings/LiveControlMap.h', [
  [/\binputContext\b/, 'use engineInputContext for live ControlMap state'],
  [/\bInputContextState\b/, 'use EngineInputContextState'],
  [/_inputContextState\b/, 'use _engineInputContextState'],
]);
checkText('tools/webview2_shared/Wv2Protocol.h', [
  [/\bhost-side\b/i, 'qualify the browser-host side of the private IPC protocol'],
]);
for (const file of [
  'frontend/src/views/osfui/keybinds/App.tsx',
  'frontend/src/views/osfui/keybinds/BindList.tsx',
  'frontend/src/views/osfui/keybinds/DetailPanel.tsx',
]) {
  checkText(file, [
    [/\binputContext\b/, 'use engineInputContext for the live ControlMap state'],
  ]);
}
checkText('packages/create-osfui/src/prompts.mjs', [
  [/Choose a surface/, 'ask for a starter type'],
  [/View ID/, 'ask for the local view name'],
]);
checkText('packages/create-osfui/src/mod-backend-templates.mjs', [
  [/\bloaded views?\b/i, 'use instantiated views for live browser objects'],
  [/\bcommands\b/i, 'generated capability copy should distinguish sends from requests'],
  [/RegisterView loads/i, 'RegisterView validates a discovered view; openOnStart instantiates it'],
  [/\bBackend (?:actions|greeting)\b/, 'qualify generated copy as mod backend'],
]);
for (const file of [
  'packages/cli/src/harness-plugin.mjs',
  'packages/create-osfui/src/mod-backend-templates.mjs',
  'packages/create-osfui/src/cli.mjs',
  'packages/create-osfui/test/scaffold.test.mjs',
]) {
  checkText(file, [
    [/\bHOST_VERSION\b/, 'use OSFUI_RELEASE_VERSION internally; HOST_VERSION is a compatibility export'],
  ]);
}
checkText('packages/create-osfui/src/cli.mjs', [
  [/\bcommands\b/i, 'generated copy should distinguish one-way sends from requests'],
  [/Send command/i, 'label the generated example as a one-way send'],
  [/fire-and-forget command/i, 'call this a fire-and-forget send endpoint'],
  [/ctx\.onCommand\s*\(\s*\(/, 'use the canonical onEndpoint mock API in generated projects'],
  [/\bBackend (?:events|actions|enabled)\b/, 'qualify generated copy as mod backend'],
]);
checkText('frontend/src/lib/bridge.ts', [
  [/named mod-backend value/i, 'use named state value because platform state shares this API'],
]);
for (const file of [
  'frontend/src/lib/keybinds/model.ts',
  'frontend/src/views/osfui/keybinds/HolderRow.tsx',
]) {
  checkText(file, [
    [/\bcontextId\b/, 'qualify hotkeyContextId or engineInputContextId on binding rows'],
    [/\bcontextLabel\b/, 'qualify hotkeyContextLabel or engineInputContextLabel on binding rows'],
    [/\bcontextNumericId\b/, 'use engineInputContextId on game-binding rows'],
  ]);
}
checkText('docs/view-toolchain.md', [
  [/\bonCommand\b/, 'use the canonical onEndpoint mock API'],
]);
for (const file of [
  'frontend/src/views/osfui/settings/App.tsx',
  'frontend/src/views/osfui/settings/Detail.tsx',
  'frontend/src/views/osfui/keybinds/App.tsx',
  'frontend/src/ui/ActionButton.tsx',
]) {
  checkText(file, [
    [/\(\s*command\s*:/, 'name internal parameters endpoint, sendEndpoint, or requestEndpoint'],
  ]);
}
checkText('packages/cli/src/cli.mjs', [
  [/\bloaded views?\b/i, 'use instantiated views for live browser objects'],
  [/--view id\b/i, 'the CLI flag selects a local view name'],
]);
checkText('packages/cli/src/config.mjs', [
  [/\bview id\b/i, 'config `id` is the local view name; qualify full view ids'],
]);
checkText('packages/cli/src/index.d.ts', [
  [/<modId>\/<id>/, 'use <modId>/<viewName> for a qualified view id'],
]);
for (const file of [
  'packages/cli/src/browser/mock-loader.js',
  'packages/cli/src/browser/mock-runtime.js',
]) {
  checkText(file, [
    [/\bharness\.ready\b/, 'use previewInitialized for the private CLI preview milestone'],
  ]);
}
checkText('packages/cli/src/browser/bootstrap.js', [
  [/\bready\s*\(\)\s*\{/, 'use previewInitialized for the private CLI preview milestone'],
  [/kind\s*:\s*['"]ready['"]/, 'reserve kind:"ready" for the web bridge handshake'],
]);
checkText('packages/cli/src/browser/shell.js', [
  [/event\.data\.kind\s*===\s*['"]ready['"]/, 'listen for the qualified preview-initialized event'],
  [/Bridge ready/, 'describe the CLI milestone as preview initialized'],
]);
checkText('frontend/src/views/osfui/settings/manifest.json', [
  [/"title"\s*:\s*"Mods"/, 'call the built-in view Mod Settings'],
]);
checkText('frontend/src/views/osfui/settings/Detail.tsx', [
  [/\bRegisteredViews\b|registered-views(?:-group|-head|-body|-empty)?|registered-view(?:-meta|-id)?/,
    'use discovered-view inventory terminology; retained i18n keys may keep registeredViews'],
]);
checkText('src/Views/Dev/DevViewReloadWorker.h', [
  [/\bbool\s+(?:overlay|world)\b/, 'dev reload targets are instantiated views; world-surface flags were removed'],
]);
checkText('frontend/src/views/osfui/settings/App.tsx', [
  [/CONTROL DECK|Exit control deck/i, 'use Mod Settings in player-facing copy'],
  [/\bMenu and HUD view\b/, 'use lowercase menu for the view kind'],
]);
for (const file of [
  'frontend/src/views/osfui/settings/Home.tsx',
  'frontend/src/views/osfui/settings/Rail.tsx',
  'frontend/src/views/osfui/settings/manifest.json',
]) {
  checkText(file, [
    [/\bMenu views?\b/, 'use lowercase menu for the view kind'],
  ]);
}
checkText('frontend/src/views/osfui/keybinds/manifest.json', [
  [/"title"\s*:\s*"Keybinds"/, 'call the built-in view Keybindings'],
]);
checkText('frontend/src/views/osfui/keybinds/App.tsx', [
  [/['"]INPUT MAP['"]/, 'use Keybindings in player-facing copy'],
]);
checkText('data/OSFUI/settings/osfui.json', [
  [/MOD MENUS/, 'use the MOD SETTINGS pause-menu label'],
  [/game-key collisions/i, 'use game-binding collisions'],
]);
checkText('packaging/nexus-page.bbcode', [
  [/\bMCM\b/, 'use Mod Settings'],
  [/Mods surface/i, 'use Mod Settings'],
  [/\bKeybinds\b/, 'use Keybindings'],
  [/MOD MENUS/, 'use MOD SETTINGS'],
  [/views\/<id>\//, 'document views/<modId>/<viewName>/ identity'],
]);

if (failures.length > 0) {
  for (const failure of failures) {
    const display = relative(root, resolve(root, failure.file)).replaceAll('\\', '/');
    console.error(`[terminology] ${display}:${failure.line}: ${failure.explanation}; found ${JSON.stringify(failure.found)}`);
  }
  process.exit(1);
}

console.log('domain terminology checks passed');
