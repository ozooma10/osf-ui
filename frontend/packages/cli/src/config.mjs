import { isAbsolute, relative, resolve, sep } from 'node:path';

import { loadConfigFromFile, normalizePath } from 'vite';

import { CONFIG_FILES, MAX_MOD_ID_LENGTH, VIEW_ID_PATTERN, isAcceptedModId } from './constants.mjs';
import { exists } from './fsutil.mjs';

const MOCK_CANDIDATES = ['osfui.mock.ts', 'osfui.mock.mts', 'osfui.mock.js', 'osfui.mock.mjs'];

function inside(root, path) {
  const child = relative(root, path);
  return child === '' || (!isAbsolute(child) && child !== '..' && !child.startsWith(`..${sep}`));
}

function relativePath(value, label) {
  if (typeof value !== 'string' || !value || isAbsolute(value) ||
      value.replaceAll('\\', '/').split('/').includes('..')) {
    throw new Error(`${label} must be a relative path inside the project.`);
  }
  return normalizePath(value);
}

function dimension(value, fallback) {
  return Number.isInteger(value) ? Math.max(1, Math.min(16384, value)) : fallback;
}

async function viteConfig(value, command) {
  const mode = command === 'serve' ? 'development' : 'production';
  const resolved = typeof value === 'function'
    ? await value({ command, mode })
    : value;
  if (resolved === undefined) return {};
  if (!resolved || typeof resolved !== 'object' || Array.isArray(resolved)) {
    throw new Error('vite must be a Vite configuration object or a function returning one.');
  }
  return resolved;
}

async function findMock(root, viewsRoot, authored) {
  let path = null;
  if (authored !== undefined) {
    path = resolve(root, relativePath(authored, 'mock'));
    if (!await exists(path)) throw new Error(`Mock file not found: ${path}`);
  } else {
    for (const name of MOCK_CANDIDATES) {
      const candidate = resolve(root, name);
      if (await exists(candidate)) { path = candidate; break; }
    }
  }
  if (path && inside(viewsRoot, path)) {
    throw new Error('The mock must live outside src/views so it cannot ship.');
  }
  return path;
}

export async function loadProject(cwd, command = 'serve') {
  const root = resolve(cwd);
  const viteCommand = command === 'serve' ? 'serve' : 'build';
  let configName = null;
  for (const name of CONFIG_FILES) {
    if (await exists(resolve(root, name))) { configName = name; break; }
  }
  if (!configName) throw new Error(`No ${CONFIG_FILES.join(' or ')} found in ${root}.`);

  const configPath = resolve(root, configName);
  const loaded = await loadConfigFromFile(
    { command: viteCommand, mode: viteCommand === 'serve' ? 'development' : 'production' },
    configPath,
    root,
    'silent',
  );
  if (!loaded?.config) throw new Error(`Could not load ${configPath}.`);
  const raw = loaded.config;
  if (!isAcceptedModId(raw.modId)) {
    throw new Error(`modId "${raw.modId || ''}" must be a safe non-empty name of at most ${MAX_MOD_ID_LENGTH} UTF-8 bytes.`);
  }

  const authoredViews = raw.views ?? (raw.view ? [raw.view] : []);
  if (!Array.isArray(authoredViews) || authoredViews.length === 0) {
    throw new Error(`${configName} must declare view or views.`);
  }

  const viewsRoot = resolve(root, 'src/views');
  const seen = new Set();
  const views = [];
  for (const authored of authoredViews) {
    if (!authored || typeof authored !== 'object' ||
        !VIEW_ID_PATTERN.test(authored.id || '') || authored.id.length > 64) {
      throw new Error('Every view name must use lowercase letters, digits, and hyphens.');
    }
    if (seen.has(authored.id)) throw new Error(`Duplicate view name "${authored.id}".`);
    seen.add(authored.id);

    const expectedSource = `src/views/${raw.modId}/${authored.id}`;
    const source = relativePath(authored.source ?? expectedSource, `view "${authored.id}" source`);
    if (source !== expectedSource) {
      throw new Error(`view "${authored.id}" source must be "${expectedSource}".`);
    }
    const entry = relativePath(authored.entry ?? 'index.html', `view "${authored.id}" entry`);
    const sourceDir = resolve(root, source);
    const entryPath = resolve(sourceDir, entry);
    if (!await exists(entryPath)) throw new Error(`View entry not found: ${entryPath}`);

    const kind = authored.kind ?? 'menu';
    if (kind !== 'menu' && kind !== 'hud') {
      throw new Error(`view "${authored.id}" kind must be "menu" or "hud".`);
    }
    views.push({
      ...authored,
      id: authored.id,
      qualifiedId: `${raw.modId}/${authored.id}`,
      title: authored.title || `${raw.modId}/${authored.id}`,
      description: authored.description || '',
      source,
      sourceDir,
      entry,
      entryPath,
      kind,
      width: dimension(authored.width, 1600),
      height: dimension(authored.height, 900),
      transparent: authored.transparent !== false,
      capturesInput: kind === 'menu' && authored.capturesInput !== false,
      pausesGame: kind === 'menu' && authored.pausesGame !== false,
    });
  }

  const mockPath = await findMock(root, viewsRoot, raw.mock);
  const outDir = resolve(root, raw.outDir ?? 'dist');
  if (inside(viewsRoot, outDir) || inside(outDir, viewsRoot) ||
      inside(configPath, outDir) || inside(outDir, configPath) ||
      (mockPath && (inside(mockPath, outDir) || inside(outDir, mockPath)))) {
    throw new Error('outDir must be separate from project inputs.');
  }

  return {
    root,
    configPath,
    modId: raw.modId,
    views,
    viewsRoot,
    outDir,
    outputViewsRoot: resolve(outDir, 'Data/SFSE/Plugins/OSF/UI/views'),
    mockPath,
    vite: await viteConfig(raw.vite, viteCommand),
  };
}

export function manifestFor(view) {
  return {
    manifestVersion: 1,
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
    debugOnly: view.debugOnly === true,
  };
}
