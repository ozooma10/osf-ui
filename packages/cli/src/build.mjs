import { access, cp, mkdir, readFile, rm, writeFile } from 'node:fs/promises';
import { resolve } from 'node:path';

import { build as viteBuild } from 'vite';

import { manifestFor } from './config.mjs';
import { sharedAssetPath } from './shared-assets.mjs';

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
      return readFile(await sharedAssetPath(id.slice(prefix.length)), 'utf8');
    },
  };
}

export async function buildProject(project, { quiet = false } = {}) {
  await rm(project.outDir, { recursive: true, force: true });
  if (await access(project.modRoot).then(() => true, () => false)) {
    await cp(project.modRoot, project.outDir, { recursive: true });
  }
  await viteBuild({
    root: resolve(project.viewsRoot, project.modId),
    base: './',
    plugins: [sharedKitPlugin()],
    build: {
      outDir: resolve(project.outputViewsRoot, project.modId),
      assetsDir: 'assets',
      emptyOutDir: false,
      rollupOptions: { input: project.views.map((view) => view.entryPath) },
    },
    logLevel: quiet ? 'error' : 'info',
  });
  for (const view of project.views) {
    const output = resolve(project.outputViewsRoot, project.modId, view.id);
    await mkdir(output, { recursive: true });
    await writeFile(resolve(output, 'manifest.json'), `${JSON.stringify(manifestFor(view), null, 2)}\n`);
  }
  return project.outDir;
}
