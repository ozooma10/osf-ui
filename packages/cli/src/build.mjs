import { access, cp, mkdir, readFile, rm, writeFile } from 'node:fs/promises';
import { dirname, resolve } from 'node:path';
import { fileURLToPath } from 'node:url';

import preact from '@preact/preset-vite';
import { build as viteBuild } from 'vite';

import { manifestFor } from './config.mjs';

const HERE = dirname(fileURLToPath(import.meta.url));

async function sharedAssetSource(name) {
  const packaged = resolve(HERE, '..', 'assets', name);
  const monorepo = resolve(HERE, '..', '..', '..', 'frontend', 'src', 'shared-kit', name);
  try { await access(packaged); return packaged; } catch { return monorepo; }
}

function sharedKitPlugin() {
  const prefix = '\0osfui-shared:';
  return {
    name: 'osfui-shared-kit',
    resolveId(source) {
      if (source === '/shared/osfui.js') return `${prefix}osfui.js`;
      if (source === '/shared/osfui.css') return `${prefix}osfui.css`;
    },
    async load(id) {
      if (!id.startsWith(prefix)) return null;
      return readFile(await sharedAssetSource(id.slice(prefix.length)), 'utf8');
    },
  };
}

export async function buildProject(project, { quiet = false } = {}) {
  await rm(project.outputViewsRoot, { recursive: true, force: true });
  await viteBuild({
    root: project.viewsRoot,
    base: './',
    plugins: [sharedKitPlugin(), preact()],
    build: {
      outDir: project.outputViewsRoot,
      emptyOutDir: false,
      rollupOptions: { input: project.views.map((view) => view.entryPath) },
    },
    logLevel: quiet ? 'error' : 'info',
  });
  for (const view of project.views) {
    const output = resolve(project.outputViewsRoot, project.modId, view.id);
    await mkdir(output, { recursive: true });
    await writeFile(resolve(output, 'manifest.json'), `${JSON.stringify(manifestFor(project, view), null, 2)}\n`);
  }
  const shared = resolve(project.outputViewsRoot, 'shared');
  await mkdir(shared, { recursive: true });
  for (const name of ['osfui.js', 'osfui.css']) {
    await cp(await sharedAssetSource(name), resolve(shared, name));
  }
  return project.outDir;
}
