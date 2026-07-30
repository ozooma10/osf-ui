import { readdir, readFile } from 'node:fs/promises';
import { exists } from './fsutil.mjs';
import { spawn } from 'node:child_process';
import { createRequire } from 'node:module';
import { extname, resolve } from 'node:path';

const TEXT = new Set(['.css', '.html', '.js', '.jsx', '.json', '.mjs', '.ts', '.tsx']);
const require = createRequire(import.meta.url);
// Egress constructs only. A URL as inert data — display text, an external
// <a href> the host opens in the player's browser, an SVG xmlns — is allowed;
// OSF UI's own settings views ship exactly those shapes, and this gate runs
// on them via `osfui check`. The runtime independently 403s every network
// fetch, so this is an early advisory, not the enforcement point.
const RULES = [
  [/\b(?:fetch|importScripts)\s*\([^)]*https?:\/\//i, 'remote HTTP URL'],
  [/\bimport\s*\(\s*["'`]https?:\/\//i, 'remote HTTP URL'],
  [/\bsendBeacon\s*\(/i, 'unsupported network or worker API'],
  [/<(?:script|img|iframe|audio|video|source|link|embed|object)\b[^>]*\b(?:src|href|data)\s*=\s*["']https?:\/\//i, 'remote HTTP URL'],
  [/\burl\(\s*["']?https?:\/\//i, 'remote HTTP URL'],
  [/@import\s+(?:url\(\s*)?["']?https?:\/\//i, 'remote HTTP URL'],
  [/\b(?:WebSocket|WebTransport|RTCPeerConnection|SharedWorker|Worker|XMLHttpRequest|EventSource)\b/, 'unsupported network or worker API'],
];

async function files(root) {
  const result = [];
  for (const entry of await readdir(root, { withFileTypes: true })) {
    const path = resolve(root, entry.name);
    if (entry.isDirectory()) result.push(...await files(path));
    else if (TEXT.has(extname(path))) result.push(path);
  }
  return result;
}

export async function checkProject(project) {
  const problems = [];
  for (const view of project.views) {
    for (const path of await files(view.sourceDir)) {
      const source = await readFile(path, 'utf8');
      for (const [pattern, label] of RULES) {
        if (pattern.test(source)) problems.push(`${path}: ${label}`);
      }
    }
  }
  if (problems.length) throw new Error(`OSF UI compatibility check failed:\n${problems.join('\n')}`);
  const tsconfig = resolve(project.root, 'tsconfig.json');
  if (await exists(tsconfig)) {
    await runTypeScript(project.root, tsconfig);
  }
  return project.views.length;
}

function runTypeScript(cwd, tsconfig) {
  const packageJson = require.resolve('typescript/package.json');
  const executable = resolve(packageJson, '..', 'lib', 'tsc.js');
  return new Promise((fulfill, reject) => {
    const child = spawn(process.execPath, [executable, '--noEmit', '-p', tsconfig], {
      cwd,
      stdio: ['ignore', 'pipe', 'pipe'],
    });
    let output = '';
    child.stdout.on('data', (chunk) => { output += chunk; });
    child.stderr.on('data', (chunk) => { output += chunk; });
    child.once('error', reject);
    child.once('exit', (code) => {
      if (code === 0) fulfill();
      else reject(new Error(`TypeScript check failed:\n${output.trim()}`));
    });
  });
}
