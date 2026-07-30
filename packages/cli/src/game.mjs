import { rmSync } from 'node:fs';
import { copyFile, cp, lstat, mkdir, readFile, readdir, rm, writeFile } from 'node:fs/promises';
import { basename, join, relative, resolve } from 'node:path';
import { createInterface } from 'node:readline/promises';
import { stdin, stdout } from 'node:process';

import { AUTHOR_MARKER, BUILD_MARKER, LOCAL_FILE } from './constants.mjs';
import { within } from './fsutil.mjs';
import { buildProject } from './build.mjs';
import { buildPapyrus } from './papyrus-build.mjs';
import { reportPapyrus } from './papyrus.mjs';

export function deploymentRoot(project, modsRoot) {
  return resolve(modsRoot, basename(project.root));
}

async function treeEntries(root, at = '', entries = []) {
  const children = await readdir(resolve(root, at), { withFileTypes: true });
  children.sort((left, right) => left.name.localeCompare(right.name));
  for (const child of children) {
    const relativePath = join(at, child.name);
    entries.push({ relativePath, directory: child.isDirectory(), symlink: child.isSymbolicLink() });
    if (child.isDirectory()) await treeEntries(root, relativePath, entries);
  }
  return entries;
}

async function existingKind(path) {
  try {
    const value = await lstat(path);
    return value.isDirectory() ? 'directory' : value.isSymbolicLink() ? 'symlink' : 'file';
  } catch (error) {
    if (error?.code === 'ENOENT') return undefined;
    throw error;
  }
}

/**
 * Make `destination` an exact copy without replacing its root directory.
 *
 * MO2's USVFS can retain vanished directory entries when a running game has
 * already enumerated them. Updating/adding first and pruning stale entries
 * last keeps the live virtual directory continuously valid while preserving
 * the exact-mirror behavior needed by full deployments.
 */
export async function mirrorTree(source, destination) {
  // The build marker is outDir bookkeeping, not mod content; keep it out of
  // the deployed MO2 folder (the prune below also removes stale copies).
  const sourceEntries = (await treeEntries(source))
    .filter((entry) => entry.relativePath !== BUILD_MARKER);
  const sourcePaths = new Set(sourceEntries.map(({ relativePath }) => relativePath));
  await mkdir(destination, { recursive: true });

  // Establish the directory shape before copying files.
  for (const entry of sourceEntries) {
    if (!entry.directory) continue;
    const path = resolve(destination, entry.relativePath);
    const kind = await existingKind(path);
    if (kind && kind !== 'directory') await rm(path, { recursive: true, force: true });
    await mkdir(path, { recursive: true });
  }

  // Copy complete build outputs over the live tree. New hashed bundles sort
  // before view HTML, and old bundles remain until the prune below.
  for (const entry of sourceEntries) {
    if (entry.directory) continue;
    const from = resolve(source, entry.relativePath);
    const to = resolve(destination, entry.relativePath);
    const kind = await existingKind(to);
    if (kind === 'directory' || entry.symlink || kind === 'symlink') {
      if (kind) await rm(to, { recursive: true, force: true });
      await cp(from, to, { force: true });
    } else {
      await copyFile(from, to);
    }
  }

  // Remove paths absent from the build deepest-first, without ever removing
  // the destination root that the running USVFS process has enumerated.
  const destinationEntries = await treeEntries(destination);
  destinationEntries.sort((left, right) => {
    const leftDepth = left.relativePath.split(/[\\/]/).length;
    const rightDepth = right.relativePath.split(/[\\/]/).length;
    return rightDepth - leftDepth;
  });
  for (const entry of destinationEntries) {
    if (!sourcePaths.has(entry.relativePath)) {
      await rm(resolve(destination, entry.relativePath), { recursive: true, force: true });
    }
  }
}

async function readLocalConfig(localPath) {
  let raw;
  try {
    raw = await readFile(localPath, 'utf8');
  } catch (error) {
    if (error?.code === 'ENOENT') return {};
    throw error;
  }
  let parsed;
  try {
    parsed = JSON.parse(raw);
  } catch (error) {
    // A malformed file must fail loudly: swallowing it falls through to the
    // interactive prompt, which then rewrites the file and destroys whatever
    // the author had in it.
    throw new Error(`${localPath} is not valid JSON: ${error.message}`);
  }
  return parsed && typeof parsed === 'object' && !Array.isArray(parsed) ? parsed : {};
}

/**
 * Persist the MO2 mods root without touching the file's other keys —
 * local.json also carries the Papyrus toolchain overrides (starfieldRoot,
 * spriggitCli, papyrusCompiler, papyrusImports; see papyrus-build.mjs), which
 * a wholesale write would destroy.
 */
export async function saveLocalModsRoot(projectRoot, modsRoot) {
  const localPath = resolve(projectRoot, LOCAL_FILE);
  const local = await readLocalConfig(localPath);
  await mkdir(resolve(localPath, '..'), { recursive: true });
  await writeFile(localPath, `${JSON.stringify({ ...local, modsRoot }, null, 2)}\n`);
}

export async function configuredDeployRoot(project, explicit) {
  if (explicit) return deploymentRoot(project, explicit);
  const local = await readLocalConfig(resolve(project.root, LOCAL_FILE));
  if (typeof local.modsRoot === 'string' && local.modsRoot) {
    return deploymentRoot(project, local.modsRoot);
  }
  if (stdin.isTTY) {
    const prompt = createInterface({ input: stdin, output: stdout });
    try {
      const answer = (await prompt.question(
        'MO2 mods directory to sync into: ',
      )).trim();
      if (answer) {
        const modsRoot = resolve(answer);
        await saveLocalModsRoot(project.root, modsRoot);
        console.log(`[osfui] Saved this local path in ${LOCAL_FILE}.`);
        return deploymentRoot(project, modsRoot);
      }
    } finally {
      prompt.close();
    }
  }
  throw new Error(
    `Game deployment is not configured. Run with --deploy "C:\\path\\to\\MO2\\mods".`,
  );
}

export async function deployBuild(project, deployRoot) {
  await mkdir(resolve(deployRoot, '..'), { recursive: true });
  await mirrorTree(project.outDir, deployRoot);
}

/**
 * Mirror only the built view assets (html/js/css and their manifests). A
 * running Starfield holds the plugin, the compiled scripts, and the native
 * DLLs open, so a hot reload must never delete or rewrite them.
 */
export async function deployViews(project, deployRoot) {
  const subpath = relative(project.outDir, project.outputViewsRoot);
  const from = resolve(project.outputViewsRoot, project.modId);
  const to = resolve(deployRoot, subpath, project.modId);
  await mkdir(resolve(to, '..'), { recursive: true });
  await mirrorTree(from, to);
}

/** True when the error is the running game holding a deployed file open. */
function isLocked(error) {
  return error?.code === 'EBUSY' || error?.code === 'EPERM' || error?.code === 'EACCES';
}

export async function startGameSync(project, server, options = {}) {
  const deployRoot = await configuredDeployRoot(project, options.deploy);
  const osfuiRoot = resolve(deployRoot, 'SFSE/Plugins/OSFUI');
  const marker = resolve(osfuiRoot, AUTHOR_MARKER);
  const markerContents = `${JSON.stringify({
    enabled: true,
    expiresAt: Math.floor(Date.now() / 1000) + (12 * 60 * 60),
    source: '@osfui/cli',
  }, null, 2)}\n`;
  const enableAuthorMode = async () => {
    await mkdir(osfuiRoot, { recursive: true });
    await writeFile(marker, markerContents);
  };
  await enableAuthorMode();

  const nativeInputs = [
    project.modRoot,
    ...(project.papyrus ? [project.papyrus.sourceDir] : []),
  ];
  let building = false;
  let pending = false;
  let fullDeploy = true;
  let nativeChanged = false;
  const sync = async () => {
    if (building) { pending = true; return; }
    building = true;
    const nativeDirty = nativeChanged;
    nativeChanged = false;
    try {
      await buildPapyrus(project);
      await buildProject(project, { quiet: true });
      if (fullDeploy) {
        try {
          await deployBuild(project, deployRoot);
        } catch (error) {
          if (!isLocked(error)) throw error;
          await deployViews(project, deployRoot);
          console.warn(
            `[osfui] ${deployRoot} is locked by the running game; deployed view assets only. ` +
            'Close Starfield and restart osfui dev to deploy the plugin and scripts.',
          );
        }
        fullDeploy = false;
      } else {
        // Hot reload touches view assets only: the game keeps the plugin and
        // the native files open, and rewriting them mid-session fails (EBUSY).
        await deployViews(project, deployRoot);
        if (nativeDirty) {
          console.log(
            '[osfui] Plugin and script sources changed. Hot reload only updates view assets — ' +
            'close Starfield and restart osfui dev to deploy them.',
          );
        }
      }
      await enableAuthorMode();
      console.log(`[osfui] Synced ${project.views.length} view(s) to ${deployRoot}`);
      // Re-warn on every sync: a .psc edit triggers this watcher, so the
      // reminder to recompile lands right when the source changed.
      await reportPapyrus(project);
    } catch (error) {
      console.error(`[osfui] Game sync failed: ${error.message}`);
    } finally {
      building = false;
      if (pending) { pending = false; void sync(); }
    }
  };
  await sync();
  server.watcher.add(project.modRoot);
  if (project.papyrus) server.watcher.add(project.papyrus.sourceDir);
  const onWatch = (path) => {
    if (typeof path === 'string' && nativeInputs.some((root) => within(root, path))) {
      nativeChanged = true;
    }
    void sync();
  };
  server.watcher.on('change', onWatch);
  server.watcher.on('add', onWatch);
  server.watcher.on('unlink', onWatch);
  const cleanup = async () => {
    try { await rm(marker, { force: true }); } catch {}
  };
  const cleanupSync = () => {
    try { rmSync(marker, { force: true }); } catch {}
  };
  process.once('SIGINT', async () => { await cleanup(); process.exit(130); });
  process.once('SIGTERM', async () => { await cleanup(); process.exit(143); });
  process.once('exit', cleanupSync);
  server.httpServer?.once('close', () => { void cleanup(); });
  console.log('[osfui] Temporary author mode enabled for this session (automatic view reload, F12 DevTools).');
  return { deployRoot, marker, cleanup };
}
