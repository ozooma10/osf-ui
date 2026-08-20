import { copyFile, mkdir, readdir, readFile, writeFile } from 'node:fs/promises';
import { dirname, extname, isAbsolute, relative, resolve } from 'node:path';
import { fileURLToPath } from 'node:url';

const HERE = dirname(fileURLToPath(import.meta.url));
const PROJECT_TEMPLATES = resolve(HERE, '..', 'templates', 'projects');
const TOKEN = /__OSFUI_[A-Z0-9_]+?__/g;
const TEXT_EXTENSIONS = new Set([
  '.cpp',
  '.css',
  '.h',
  '.html',
  '.js',
  '.json',
  '.lua',
  '.md',
  '.mjs',
  '.ps1',
  '.psc',
  '.ts',
]);

const words = (value) => value.split(/[^a-zA-Z0-9]+/).filter(Boolean);

export const pascalIdentifier = (value) => {
  const joined = words(value).map((word) => word[0].toUpperCase() + word.slice(1)).join('');
  if (!joined) return 'MyMod';
  // Papyrus ScriptName identifiers must begin with a letter; mod ids do not.
  return /^[A-Za-z]/.test(joined) ? joined : `Mod${joined}`;
};

const displayName = (modId) => words(modId).join(' ') || 'My Mod';

function renderTokens(source, values, context) {
  return source.replace(TOKEN, (token) => {
    if (!Object.hasOwn(values, token)) {
      throw new Error(`Unknown project-template token ${token} in ${context}.`);
    }
    return values[token];
  });
}

function assertWithin(root, path) {
  const child = relative(root, path);
  if (child.startsWith('..') || isAbsolute(child)) {
    throw new Error(`Project-template path escapes its destination: ${path}`);
  }
}

async function renderDirectory(sourceRoot, source, destinationRoot, destination, values, pathValues) {
  for (const entry of await readdir(source, { withFileTypes: true })) {
    const sourcePath = resolve(source, entry.name);
    const templateRelative = relative(sourceRoot, sourcePath).replaceAll('\\', '/');
    const templateName = entry.name === '_gitignore' ? '.gitignore' : entry.name;
    const outputName = renderTokens(templateName, pathValues, templateRelative);
    const destinationPath = resolve(destination, outputName);
    assertWithin(destinationRoot, destinationPath);

    if (entry.isDirectory()) {
      await mkdir(destinationPath, { recursive: true });
      await renderDirectory(
        sourceRoot,
        sourcePath,
        destinationRoot,
        destinationPath,
        values,
        pathValues,
      );
      continue;
    }
    if (!entry.isFile()) throw new Error(`Unsupported project-template entry: ${sourcePath}`);

    await mkdir(dirname(destinationPath), { recursive: true });
    if (!TEXT_EXTENSIONS.has(extname(entry.name)) && entry.name !== '_gitignore') {
      await copyFile(sourcePath, destinationPath);
      continue;
    }
    const content = await readFile(sourcePath, 'utf8');
    await writeFile(destinationPath, renderTokens(content, values, templateRelative));
  }
}

export async function renderProjectTemplate(root, options) {
  const preset = `${options.surface}-${options.integration}`;
  const sourceRoot = resolve(PROJECT_TEMPLATES, preset);
  const pluginName = pascalIdentifier(options.modId);
  const values = {
    '__OSFUI_DISPLAY_NAME__': displayName(options.modId),
    '__OSFUI_MOD_ID__': options.modId,
    '__OSFUI_PLUGIN_NAME__': pluginName,
    '__OSFUI_PROJECT_NAME__': options.projectName,
    '__OSFUI_RELEASE_VERSION__': options.releaseVersion,
    '__OSFUI_SCRIPT_NAME__': `${pluginName}OSFUI`,
    '__OSFUI_VIEW_ID__': options.view,
    '__OSFUI_VIEW_TITLE__': options.view.replaceAll('-', ' '),
  };
  const pathValues = {
    '__OSFUI_MOD_ID__': options.modId,
    '__OSFUI_SCRIPT_NAME__': `${pluginName}OSFUI`,
    '__OSFUI_VIEW_ID__': options.view,
  };

  await renderDirectory(sourceRoot, sourceRoot, root, root, values, pathValues);
}
