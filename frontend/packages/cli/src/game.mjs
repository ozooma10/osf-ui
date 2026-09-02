import { rmSync } from 'node:fs';
import { copyFile, lstat, mkdir, readFile, readdir, rm, writeFile } from 'node:fs/promises';
import { basename, join, resolve } from 'node:path';
import { createInterface } from 'node:readline/promises';
import { stdin, stdout } from 'node:process';

import { AUTHOR_MARKER, BUILD_MARKER, LOCAL_FILE } from './constants.mjs';
import { buildProject } from './build.mjs';

export function deploymentRoot(project, modsRoot) {
  return resolve(modsRoot, basename(project.root));
}

async function entries(root, at = '', result = []) {
  for (const entry of await readdir(resolve(root, at), { withFileTypes: true })) {
    const path = join(at, entry.name);
    result.push({ path, directory: entry.isDirectory() });
    if (entry.isDirectory()) await entries(root, path, result);
  }
  return result;
}

async function kind(path) {
  try { return (await lstat(path)).isDirectory() ? 'directory' : 'file'; }
  catch (error) { if (error?.code === 'ENOENT') return null; throw error; }
}

export async function mirrorTree(source, destination) {
  const sourceEntries = (await entries(source)).filter((entry) => entry.path !== BUILD_MARKER);
  const wanted = new Set(sourceEntries.map((entry) => entry.path));
  await mkdir(destination, { recursive: true });
  for (const entry of sourceEntries.filter((entry) => entry.directory)) {
    const target = resolve(destination, entry.path);
    if (await kind(target) === 'file') await rm(target, { force: true });
    await mkdir(target, { recursive: true });
  }
  for (const entry of sourceEntries.filter((entry) => !entry.directory)) {
    const target = resolve(destination, entry.path);
    if (await kind(target) === 'directory') await rm(target, { recursive: true, force: true });
    await copyFile(resolve(source, entry.path), target);
  }
  const stale = await entries(destination);
  stale.sort((left, right) => right.path.length - left.path.length);
  for (const entry of stale) {
    if (!wanted.has(entry.path)) await rm(resolve(destination, entry.path), { recursive: true, force: true });
  }
}

async function readLocal(projectRoot) {
  try { return JSON.parse(await readFile(resolve(projectRoot, LOCAL_FILE), 'utf8')); }
  catch (error) { if (error?.code === 'ENOENT') return {}; throw error; }
}

async function deployRootFor(project, explicit) {
  let modsRoot = explicit;
  const local = await readLocal(project.root);
  if (!modsRoot && typeof local.modsRoot === 'string') modsRoot = local.modsRoot;
  if (!modsRoot && stdin.isTTY) {
    const prompt = createInterface({ input: stdin, output: stdout });
    try { modsRoot = (await prompt.question('MO2 mods directory to sync into: ')).trim(); }
    finally { prompt.close(); }
    if (modsRoot) {
      const path = resolve(project.root, LOCAL_FILE);
      await mkdir(resolve(path, '..'), { recursive: true });
      await writeFile(path, `${JSON.stringify({ ...local, modsRoot: resolve(modsRoot) }, null, 2)}\n`);
    }
  }
  if (!modsRoot) throw new Error('Game deployment is not configured. Use --deploy "C:\\path\\to\\MO2\\mods".');
  return deploymentRoot(project, resolve(modsRoot));
}

export async function startGameSync(project, server, options = {}) {
  const deployRoot = await deployRootFor(project, options.deploy);
  const marker = resolve(deployRoot, 'Data/SFSE/Plugins/OSF/UI', AUTHOR_MARKER);
  let building = false;
  let pending = false;
  const sync = async () => {
    if (building) { pending = true; return; }
    building = true;
    try {
      await buildProject(project, { quiet: true });
      await mirrorTree(project.outDir, deployRoot);
      await mkdir(resolve(marker, '..'), { recursive: true });
      await writeFile(marker, `${JSON.stringify({
        enabled: true,
        expiresAt: Math.floor(Date.now() / 1000) + 12 * 60 * 60,
        source: '@osfui/cli',
      }, null, 2)}\n`);
      console.log(`[osfui] Synced ${project.views.length} view(s) to ${deployRoot}`);
    } catch (error) {
      console.error(`[osfui] Game sync failed: ${error.message}`);
    } finally {
      building = false;
      if (pending) { pending = false; void sync(); }
    }
  };
  await sync();
  const onChange = () => { void sync(); };
  server.watcher.add(project.viewsRoot);
  server.watcher.on('change', onChange);
  server.watcher.on('add', onChange);
  server.watcher.on('unlink', onChange);
  const cleanup = async () => { try { await rm(marker, { force: true }); } catch {} };
  process.once('SIGINT', async () => { await cleanup(); process.exit(130); });
  process.once('SIGTERM', async () => { await cleanup(); process.exit(143); });
  process.once('exit', () => { try { rmSync(marker, { force: true }); } catch {} });
  server.httpServer?.once('close', () => { void cleanup(); });
  console.log('[osfui] Temporary developer mode enabled for this session.');
  return { deployRoot, marker, cleanup };
}
