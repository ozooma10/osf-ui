#!/usr/bin/env node
import { mkdir } from 'node:fs/promises';
import { basename, resolve } from 'node:path';

import preact from '@preact/preset-vite';
import { createServer } from 'vite';

import { buildProject } from './build.mjs';
import { checkProject } from './check.mjs';
import { loadProject } from './config.mjs';
import { startGameSync } from './game.mjs';
import { harnessPlugin } from './harness-plugin.mjs';
import { writeZip } from './zip.mjs';

function parse(argv) {
  const options = { _: [] };
  for (let index = 0; index < argv.length; index++) {
    const value = argv[index];
    if (!value.startsWith('--')) options._.push(value);
    else if (value === '--game' || value === '--help' || value === '--version') options[value.slice(2)] = true;
    else options[value.slice(2)] = argv[++index];
  }
  return options;
}

function help() {
  console.log(`OSF UI view authoring

Usage:
  osfui dev [--view id] [--game] [--deploy path]
  osfui check
  osfui build
  osfui package [--output file]
  osfui doctor

F11 reloads the active view and F12 opens WebView2 DevTools while dev --game is running.`);
}

async function main() {
  const options = parse(process.argv.slice(2));
  const command = options._[0] || 'dev';
  if (options.help) { help(); return; }
  if (options.version) { console.log('0.1.0'); return; }
  const project = await loadProject(process.cwd(), command === 'dev' ? 'serve' : 'build');
  if (command === 'dev') {
    const view = options.view
      ? project.views.find((candidate) => candidate.id === options.view)
      : project.views[0];
    if (!view) throw new Error(`Unknown view "${options.view}".`);
    const server = await createServer({
      root: project.viewsRoot,
      base: '/',
      plugins: [harnessPlugin(project, view), preact()],
      server: {
        host: options.host || '127.0.0.1',
        port: Number(options.port) || 5173,
        open: options.open === 'false' ? false : '/__osfui/',
        // Vite can canonicalize Windows temp/project paths through their 8.3
        // aliases and then reject its own root. The author server binds to
        // loopback by default, so disable that redundant filesystem check.
        fs: { strict: false },
      },
    });
    await server.listen();
    server.printUrls();
    console.log(`[osfui] Previewing ${view.qualifiedId}; edits hot-reload automatically.`);
    if (options.game) await startGameSync(project, server, options);
    return;
  }
  if (command === 'check') {
    const count = await checkProject(project);
    console.log(`[osfui] ${count} view(s) passed compatibility checks.`);
    return;
  }
  if (command === 'doctor') {
    console.log(`[osfui] Node ${process.version}`);
    console.log(`[osfui] Project ${project.root}`);
    console.log(`[osfui] ${project.views.length} configured view(s)`);
    console.log('[osfui] Project configuration is valid.');
    return;
  }
  if (command === 'build' || command === 'package') {
    await checkProject(project);
    await buildProject(project);
    if (command === 'build') {
      console.log(`[osfui] Built ${project.views.length} view(s) in ${project.outDir}`);
      return;
    }
    const release = resolve(project.root, 'release');
    await mkdir(release, { recursive: true });
    const destination = resolve(options.output || resolve(release, `${basename(project.root)}.zip`));
    await writeZip(project.outDir, destination);
    console.log(`[osfui] Package ready: ${destination}`);
    return;
  }
  help();
  throw new Error(`Unknown command "${command}".`);
}

main().catch((error) => {
  console.error(`[osfui] ${error.message}`);
  process.exitCode = 1;
});
