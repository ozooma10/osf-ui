#!/usr/bin/env node

import { spawn } from 'node:child_process';
import { resolve } from 'node:path';

import { createViewHarness } from './view-harness-lib.mjs';

function usage() {
  console.log(`OSF UI generic browser harness

Usage:
  npm run dev:view -- <views/mod.id/view> [options]
  node scripts/view-harness.mjs <views/mod.id/view> [options]

Options:
  --port <number>  Local port (default 8081; use 0 to choose a free port)
  --no-open        Do not open the default browser
  --help           Show this help

The view directory must contain manifest.json and its entry HTML. Optional
osfui.mock.json provides state and custom request responses.`);
}

function parseArgs(argv) {
  let viewPath = '';
  let port = 8081;
  let open = true;
  for (let index = 0; index < argv.length; index++) {
    const arg = argv[index];
    if (arg === '--help' || arg === '-h') return { help: true };
    if (arg === '--no-open') {
      open = false;
      continue;
    }
    if (arg === '--port') {
      const value = Number(argv[++index]);
      if (!Number.isInteger(value) || value < 0 || value > 65535) {
        throw new Error('--port must be an integer from 0 to 65535.');
      }
      port = value;
      continue;
    }
    if (arg.startsWith('-')) throw new Error(`Unknown option: ${arg}`);
    if (viewPath) throw new Error('Pass exactly one view directory.');
    viewPath = resolve(arg);
  }
  if (!viewPath) throw new Error('A view directory is required.');
  return { viewPath, port, open };
}

function openBrowser(url) {
  let command;
  let args;
  if (process.platform === 'win32') {
    command = 'cmd.exe';
    args = ['/d', '/s', '/c', 'start', '""', url];
  } else if (process.platform === 'darwin') {
    command = 'open';
    args = [url];
  } else {
    command = 'xdg-open';
    args = [url];
  }
  const child = spawn(command, args, { detached: true, stdio: 'ignore' });
  child.unref();
  child.on('error', (error) => {
    console.warn(`[view-harness] Could not open the browser: ${error.message}`);
  });
}

let parsed;
try {
  parsed = parseArgs(process.argv.slice(2));
} catch (error) {
  console.error(`[view-harness] ${error.message}\n`);
  usage();
  process.exit(1);
}

if (parsed.help) {
  usage();
  process.exit(0);
}

let harness;
try {
  harness = await createViewHarness(parsed.viewPath, { port: parsed.port });
} catch (error) {
  console.error(`[view-harness] ${error.message}`);
  process.exit(1);
}

console.log(`[view-harness] ${harness.definition.qualifiedId}`);
console.log(`[view-harness] serving ${harness.definition.viewsRoot}`);
console.log(`[view-harness] ${harness.url}`);
console.log('[view-harness] save a view file to reload; press Ctrl+C to stop');
if (parsed.open) openBrowser(harness.url);

let closing = false;
async function close() {
  if (closing) return;
  closing = true;
  await harness.close();
  process.exit(0);
}
process.on('SIGINT', close);
process.on('SIGTERM', close);
