#!/usr/bin/env node
import { parseArgs } from 'node:util';

import { createServer } from 'vite';

import { buildProject } from './build.mjs';
import { checkProject } from './check.mjs';
import { loadProject } from './config.mjs';
import { CLI_VERSION } from './constants.mjs';
import { devServerConfig } from './dev.mjs';
import { startGameSync } from './game.mjs';

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
  osfui dev [--view name] [--game] [--deploy path] [--host address] [--port n] [--open false]
  osfui check
  osfui build
  osfui doctor

Instantiated views reload automatically and F12 opens WebView2 DevTools while dev --game is running.`);
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
    console.log(`[osfui] ${count} view(s) passed authoring checks.`);
    return;
  }
  if (command === 'doctor') {
    console.log(`[osfui] Node ${process.version}`);
    console.log(`[osfui] Project ${project.root}`);
    console.log(`[osfui] ${project.views.length} configured view(s)`);
    console.log('[osfui] Project configuration is valid.');
    return;
  }
  if (command === 'build') {
    await checkProject(project);
    await buildProject(project);
    console.log(`[osfui] Built ${project.views.length} view(s) in ${project.outDir}`);
    return;
  }
  help();
  throw new Error(`Unknown command "${command}".`);
}

main().catch((error) => {
  console.error(`[osfui] ${error.message}`);
  process.exitCode = 1;
});
