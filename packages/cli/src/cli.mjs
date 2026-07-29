#!/usr/bin/env node
import { mkdir } from 'node:fs/promises';
import { basename, resolve } from 'node:path';
import { parseArgs } from 'node:util';

import { createServer } from 'vite';

import { buildProject } from './build.mjs';
import { checkProject } from './check.mjs';
import { loadProject } from './config.mjs';
import { CLI_VERSION } from './constants.mjs';
import { devServerConfig } from './dev.mjs';
import { startGameSync } from './game.mjs';
import { reportPapyrus } from './papyrus.mjs';
import { writeZip } from './zip.mjs';

function parse(argv) {
  const { values, positionals } = parseArgs({
    args: argv,
    allowPositionals: true,
    strict: true,
    options: {
      deploy: { type: 'string' },
      game: { type: 'boolean' },
      help: { type: 'boolean', short: 'h' },
      host: { type: 'string' },
      open: { type: 'string' },
      output: { type: 'string' },
      port: { type: 'string' },
      version: { type: 'boolean', short: 'v' },
      view: { type: 'string' },
    },
  });
  return { ...values, _: positionals };
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
  if (options.version) { console.log(CLI_VERSION); return; }
  const project = await loadProject(process.cwd(), command === 'dev' ? 'serve' : 'build');
  if (command === 'dev') {
    const view = options.view
      ? project.views.find((candidate) => candidate.id === options.view)
      : project.views[0];
    if (!view) throw new Error(`Unknown view "${options.view}".`);
    const server = await createServer(await devServerConfig(project, view, options));
    await server.listen();
    server.printUrls();
    console.log(`[osfui] Previewing ${view.qualifiedId}; edits hot-reload automatically.`);
    if (options.game) await startGameSync(project, server, options);
    return;
  }
  if (command === 'check') {
    const count = await checkProject(project);
    console.log(`[osfui] ${count} view(s) passed compatibility checks.`);
    await reportPapyrus(project);
    return;
  }
  if (command === 'doctor') {
    console.log(`[osfui] Node ${process.version}`);
    console.log(`[osfui] Project ${project.root}`);
    console.log(`[osfui] ${project.views.length} configured view(s)`);
    await reportPapyrus(project);
    console.log('[osfui] Project configuration is valid.');
    return;
  }
  if (command === 'build' || command === 'package') {
    await checkProject(project);
    await reportPapyrus(project);
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
