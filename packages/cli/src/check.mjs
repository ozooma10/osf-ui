import Ajv2020 from 'ajv/dist/2020.js';
import { readdir, readFile } from 'node:fs/promises';
import { exists } from './fsutil.mjs';
import { spawn } from 'node:child_process';
import { createRequire } from 'node:module';
import { basename, extname, resolve } from 'node:path';
import { manifestFor } from './config.mjs';

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

let validatorsPromise;

async function schemaValidators() {
  if (!validatorsPromise) {
    validatorsPromise = Promise.all([
      readFile(new URL('../assets/manifest.schema.json', import.meta.url), 'utf8'),
      readFile(new URL('../assets/settings-schema.schema.json', import.meta.url), 'utf8'),
    ]).then(([manifestSource, settingsSource]) => {
      const ajv = new Ajv2020({ allErrors: true, strict: false });
      return {
        manifest: ajv.compile(JSON.parse(manifestSource)),
        settings: ajv.compile(JSON.parse(settingsSource)),
      };
    });
  }
  return validatorsPromise;
}

function schemaErrors(validate) {
  return (validate.errors ?? []).map((error) => {
    const at = error.instancePath || '/';
    return `${at} ${error.message}`;
  }).join('; ');
}

function validateDocument(validate, document, label) {
  if (!validate(document)) {
    throw new Error(`${label} does not match the OSF UI 2.0 schema: ${schemaErrors(validate)}`);
  }
}

async function validateAuthorContent(project) {
  const validators = await schemaValidators();
  for (const view of project.views) {
    validateDocument(validators.manifest, manifestFor(view), `view "${view.qualifiedId}"`);
  }

  const settingsRoot = resolve(project.modRoot, 'SFSE/Plugins/OSFUI/settings');
  if (!await exists(settingsRoot)) return;
  for (const entry of await readdir(settingsRoot, { withFileTypes: true })) {
    if (!entry.isFile() || extname(entry.name).toLowerCase() !== '.json') continue;
    const path = resolve(settingsRoot, entry.name);
    let document;
    try {
      document = JSON.parse(await readFile(path, 'utf8'));
    } catch (error) {
      throw new Error(`${path} is not valid JSON: ${error.message}`);
    }
    validateDocument(validators.settings, document, path);
    const stem = basename(entry.name, extname(entry.name));
    if (document.id !== undefined && document.id !== stem) {
      throw new Error(`${path} declares id "${document.id}" but its filename owns id "${stem}".`);
    }
  }
}

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
  await validateAuthorContent(project);
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
