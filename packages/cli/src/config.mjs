import { access } from 'node:fs/promises';
import { extname, isAbsolute, resolve, sep } from 'node:path';
import { loadConfigFromFile, normalizePath } from 'vite';

import {
  CONFIG_FILE,
  MOD_ID_PATTERN,
  VIEW_ID_PATTERN,
} from './constants.mjs';

const exists = (path) => access(path).then(() => true, () => false);

/**
 * Candidate mock files probed, in priority order, when the config does not
 * name one. `.json` last: a module can express everything a fixture can, so
 * a project that has both meant the module.
 */
const MOCK_CANDIDATES = [
  'osfui.mock.ts',
  'osfui.mock.mts',
  'osfui.mock.js',
  'osfui.mock.mjs',
  'osfui.mock.json',
];

/** 'module' | 'json' from a mock file's extension. */
function mockKindFor(path) {
  return extname(path).toLowerCase() === '.json' ? 'json' : 'module';
}

/**
 * Resolve the project's dev mock. Explicit `mock:` must exist (a typo would
 * otherwise silently disable the mock — the historical default pointed at a
 * file the scaffolder never wrote, and nobody noticed). Without `mock:`, the
 * first existing candidate wins; a project without any mock is fine.
 */
async function resolveMock(root, viewsRoot, raw) {
  let mockPath = null;
  if (raw !== undefined) {
    validateRelative(raw, 'mock');
    mockPath = resolve(root, raw);
    if (!await exists(mockPath)) {
      throw new Error(`Mock file not found: ${mockPath}`);
    }
  } else {
    for (const candidate of MOCK_CANDIDATES) {
      const path = resolve(root, candidate);
      if (await exists(path)) { mockPath = path; break; }
    }
  }
  if (mockPath === null) return { mockPath: null, mockKind: 'none' };
  if (mockPath.startsWith(viewsRoot + sep)) {
    throw new Error(
      'The mock must live at the project root, outside src/views, so it can never ship with the views.',
    );
  }
  return { mockPath, mockKind: mockKindFor(mockPath) };
}

function integer(value, fallback) {
  return Number.isInteger(value) ? Math.max(1, Math.min(16384, value)) : fallback;
}

function validateRelative(value, label) {
  if (typeof value !== 'string' || !value || isAbsolute(value) ||
      value.replaceAll('\\', '/').split('/').includes('..')) {
    throw new Error(`${label} must be a relative path that stays inside the project.`);
  }
  return normalizePath(value);
}

export async function loadProject(cwd, command = 'serve') {
  const root = resolve(cwd);
  const configPath = resolve(root, CONFIG_FILE);
  if (!await exists(configPath)) {
    throw new Error(`No ${CONFIG_FILE} found in ${root}.`);
  }
  const loaded = await loadConfigFromFile(
    { command, mode: command === 'serve' ? 'development' : 'production' },
    configPath,
    root,
    'silent',
  );
  if (!loaded?.config) throw new Error(`Could not load ${configPath}.`);
  const raw = loaded.config;
  if (!MOD_ID_PATTERN.test(raw.modId || '') || raw.modId.length > 64) {
    throw new Error(`modId "${raw.modId || ''}" must be <author>.<modname> using lowercase letters, digits, and hyphens.`);
  }
  const authored = raw.views ?? (raw.view ? [raw.view] : []);
  if (!Array.isArray(authored) || authored.length === 0) {
    throw new Error(`${CONFIG_FILE} must declare view or views.`);
  }
  const seen = new Set();
  const views = [];
  for (const item of authored) {
    if (!item || typeof item !== 'object' || !VIEW_ID_PATTERN.test(item.id || '') ||
        item.id.length > 64) {
      throw new Error('Every view id must use lowercase letters, digits, and hyphens.');
    }
    if (seen.has(item.id)) throw new Error(`Duplicate view id "${item.id}".`);
    seen.add(item.id);
    const kind = item.kind === 'hud' ? 'hud' : 'menu';
    const expectedSource = `src/views/${raw.modId}/${item.id}`;
    const source = validateRelative(
      item.source ?? expectedSource,
      `view "${item.id}" source`,
    );
    if (source !== expectedSource) {
      throw new Error(
        `view "${item.id}" source must be "${expectedSource}" so development and in-game URLs match.`,
      );
    }
    const entry = validateRelative(item.entry ?? 'index.html', `view "${item.id}" entry`);
    const sourceDir = resolve(root, source);
    const entryPath = resolve(sourceDir, entry);
    if (!await exists(entryPath)) throw new Error(`View entry not found: ${entryPath}`);
    const nativeBridge = item.permissions?.nativeBridge !== false;
    views.push({
      ...item,
      id: item.id,
      qualifiedId: `${raw.modId}/${item.id}`,
      title: item.title || `${raw.modId}/${item.id}`,
      description: item.description || '',
      source,
      sourceDir,
      entry,
      entryPath,
      kind,
      width: integer(item.width, 1600),
      height: integer(item.height, 900),
      transparent: item.transparent !== false,
      capturesInput: kind === 'menu' && item.capturesInput !== false,
      pausesGame: kind === 'menu' && item.pausesGame !== false,
      permissions: {
        nativeBridge,
        filesystem: false,
        network: false,
      },
    });
  }
  const viewsRoot = resolve(root, 'src/views');
  const { mockPath, mockKind } = await resolveMock(root, viewsRoot, raw.mock);
  return {
    root,
    configPath,
    modId: raw.modId,
    views,
    viewsRoot,
    outDir: resolve(root, raw.outDir || 'dist'),
    outputViewsRoot: resolve(root, raw.outDir || 'dist', 'SFSE/Plugins/OSFUI/views'),
    mockPath,
    mockKind,
  };
}

export function manifestFor(project, view) {
  return {
    id: view.id,
    title: view.title,
    description: view.description,
    entry: view.entry,
    kind: view.kind,
    width: view.width,
    height: view.height,
    transparent: view.transparent,
    capturesInput: view.capturesInput,
    pausesGame: view.pausesGame,
    openOnStart: view.openOnStart === true,
    order: Number.isInteger(view.order) ? view.order : 0,
    hub: view.hub !== false,
    debugOnly: view.debugOnly === true,
    readySignal: view.readySignal === true,
    ...(view.targetVersion ? { targetVersion: view.targetVersion } : {}),
    permissions: view.permissions,
  };
}
