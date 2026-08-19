import { spawn } from 'node:child_process';
import { createRequire } from 'node:module';
import { readdir, readFile } from 'node:fs/promises';
import { extname, resolve } from 'node:path';

import { exists } from './fsutil.mjs';

const require = createRequire(import.meta.url);
const TEXT = new Set(['.css', '.html', '.js', '.jsx', '.json', '.mjs', '.ts', '.tsx']);
const UNSUPPORTED = [
  /\b(?:fetch|importScripts)\s*\([^)]*https?:\/\//i,
  /\bimport\s*\(\s*["'`]https?:\/\//i,
  /\b(?:sendBeacon|WebSocket|WebTransport|RTCPeerConnection|SharedWorker|Worker|XMLHttpRequest|EventSource)\b/,
  /<(?:script|img|iframe|audio|video|source|link|embed|object)\b[^>]*\b(?:src|href|data)\s*=\s*["']https?:\/\//i,
  /\burl\(\s*["']?https?:\/\//i,
  /@import\s+(?:url\(\s*)?["']?https?:\/\//i,
];

async function sourceFiles(root) {
  const result = [];
  for (const entry of await readdir(root, { withFileTypes: true })) {
    const path = resolve(root, entry.name);
    if (entry.isDirectory()) result.push(...await sourceFiles(path));
    else if (TEXT.has(extname(path))) result.push(path);
  }
  return result;
}

function runTypeScript(cwd, tsconfig) {
  const executable = resolve(require.resolve('typescript/package.json'), '..', 'lib', 'tsc.js');
  return new Promise((fulfill, reject) => {
    const child = spawn(process.execPath, [executable, '--noEmit', '-p', tsconfig], {
      cwd,
      stdio: ['ignore', 'pipe', 'pipe'],
    });
    let output = '';
    child.stdout.on('data', (chunk) => { output += chunk; });
    child.stderr.on('data', (chunk) => { output += chunk; });
    child.once('error', reject);
    child.once('exit', (code) => code === 0
      ? fulfill()
      : reject(new Error(`TypeScript check failed:\n${output.trim()}`)));
  });
}

export async function checkProject(project) {
  const problems = [];
  for (const view of project.views) {
    for (const path of await sourceFiles(view.sourceDir)) {
      const source = await readFile(path, 'utf8');
      if (UNSUPPORTED.some((pattern) => pattern.test(source))) {
        problems.push(`${path}: unsupported network or worker API`);
      }
    }
  }
  if (problems.length) throw new Error(`OSF UI compatibility check failed:\n${problems.join('\n')}`);
  const tsconfig = resolve(project.root, 'tsconfig.json');
  if (await exists(tsconfig)) await runTypeScript(project.root, tsconfig);
  return project.views.length;
}
