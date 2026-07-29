import { rmSync } from 'node:fs';
import { cp, mkdir, readFile, rm, writeFile } from 'node:fs/promises';
import { basename, resolve } from 'node:path';
import { createInterface } from 'node:readline/promises';
import { stdin, stdout } from 'node:process';

import { AUTHOR_MARKER, LOCAL_FILE } from './constants.mjs';
import { buildProject } from './build.mjs';
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

  let building = false;
  let pending = false;
  const sync = async () => {
    if (building) { pending = true; return; }
    building = true;
    try {
      await buildProject(project, { quiet: true });
      await deployBuild(project, deployRoot);
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
  server.watcher.on('change', sync);
  server.watcher.on('add', sync);
  server.watcher.on('unlink', sync);
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
