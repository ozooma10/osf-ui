import { rmSync } from 'node:fs';
import { cp, mkdir, readFile, rm, writeFile } from 'node:fs/promises';
import { basename, isAbsolute, relative, resolve } from 'node:path';
import { createInterface } from 'node:readline/promises';
import { stdin, stdout } from 'node:process';

import { AUTHOR_MARKER, LOCAL_FILE } from './constants.mjs';
import { buildProject } from './build.mjs';
import { buildPapyrus } from './papyrus-build.mjs';
import { reportPapyrus } from './papyrus.mjs';

export function deploymentRoot(project, modsRoot) {
  return resolve(modsRoot, basename(project.root));
}

async function configuredDeployRoot(project, explicit) {
  if (explicit) return deploymentRoot(project, explicit);
  try {
    const local = JSON.parse(await readFile(resolve(project.root, LOCAL_FILE), 'utf8'));
    if (typeof local.modsRoot === 'string' && local.modsRoot) {
      return deploymentRoot(project, local.modsRoot);
    }
    // Before 0.2, deployRoot named the final mod directory. Keep existing
    // projects working while all newly saved paths use the MO2 mods directory.
    if (typeof local.deployRoot === 'string' && local.deployRoot) return resolve(local.deployRoot);
  } catch {}
  if (stdin.isTTY) {
    const prompt = createInterface({ input: stdin, output: stdout });
    try {
      const answer = (await prompt.question(
        'MO2 mods directory to sync into: ',
      )).trim();
      if (answer) {
        const modsRoot = resolve(answer);
        const localPath = resolve(project.root, LOCAL_FILE);
        await mkdir(resolve(localPath, '..'), { recursive: true });
        await writeFile(localPath, `${JSON.stringify({ modsRoot }, null, 2)}\n`);
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
  await rm(deployRoot, { recursive: true, force: true });
  await mkdir(resolve(deployRoot, '..'), { recursive: true });
  await cp(project.outDir, deployRoot, { recursive: true });
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
  await rm(to, { recursive: true, force: true });
  await mkdir(resolve(to, '..'), { recursive: true });
  await cp(from, to, { recursive: true });
}

function within(root, path) {
  const child = relative(root, path);
  return !isAbsolute(child) && !child.startsWith('..');
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
  console.log('[osfui] Temporary author mode enabled for this session (F11 reload, F12 DevTools).');
  return { deployRoot, marker, cleanup };
}
