import { mkdir, readFile, readdir, rm, writeFile } from 'node:fs/promises';
import { resolve } from 'node:path';

import { build as viteBuild } from 'vite';

import { BUILD_MARKER, CLI_VERSION } from './constants.mjs';
import { exists } from './fsutil.mjs';
import { manifestFor } from './config.mjs';
import { sharedAssetPath } from './shared-assets.mjs';

function sharedKitPlugin() {
  const prefix = '\0osfui-shared:';
  return {
    name: 'osfui-shared-kit',
    resolveId(source) {
      if (source === '/shared/osfui.js') return `${prefix}osfui.js`;
      if (source === '/shared/osfui.css') return `${prefix}osfui.css`;
      if (source === '/shared/gamepadnav.js') return `${prefix}gamepadnav.js`;
    },
    async load(id) {
      if (!id.startsWith(prefix)) return null;
      return readFile(await sharedAssetPath(id.slice(prefix.length)), 'utf8');
    },
  };
}

/**
 * A configured outDir may legitimately escape the project (a monorepo's
 * shared build directory is a tested contract), so the path alone cannot
 * prove the rm below is safe. Only an empty directory, a missing one, or one
 * carrying the marker a previous build wrote may be cleaned.
 */
async function assertRemovableOutDir(outDir) {
  let entries;
  try {
    entries = await readdir(outDir);
  } catch (error) {
    if (error?.code === 'ENOENT') return;
    throw error;
  }
  if (entries.length === 0 || entries.includes(BUILD_MARKER)) return;
  throw new Error(
    `Refusing to delete ${outDir}: it is not empty and was not written by osfui build ` +
    `(no ${BUILD_MARKER}). Point outDir at a dedicated directory, or empty this one yourself.`,
  );
}

export async function buildProject(project, { quiet = false } = {}) {
  await assertRemovableOutDir(project.outDir);
  await rm(project.outDir, { recursive: true, force: true });
  await mkdir(project.outDir, { recursive: true });
  // Written first, so even an aborted build leaves the directory reclaimable.
  await writeFile(
    resolve(project.outDir, BUILD_MARKER),
    `${JSON.stringify({ source: '@osfui/cli', version: CLI_VERSION }, null, 2)}\n`,
  );
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
