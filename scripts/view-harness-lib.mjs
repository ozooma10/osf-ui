import {
  createReadStream,
  existsSync,
  promises as fs,
  watch,
} from 'node:fs';
import { createServer } from 'node:http';
import {
  basename,
  dirname,
  extname,
  isAbsolute,
  relative,
  resolve,
  sep,
} from 'node:path';
import { fileURLToPath } from 'node:url';

import {
  BOOTSTRAP_JS,
  HARNESS_CSS,
  HARNESS_HTML,
  HARNESS_JS,
} from './view-harness-assets.mjs';

const SCRIPT_DIR = dirname(fileURLToPath(import.meta.url));
const REPO_ROOT = resolve(SCRIPT_DIR, '..');
const DEFAULT_FIXTURE = Object.freeze({ state: {}, requests: {}, locales: {} });
const MOD_ID = /^(?:osfui|[a-z0-9-]+\.[a-z0-9-]+)$/;
const VIEW_NAME = /^[a-z0-9-]+$/;

const MIME = new Map([
  ['.avif', 'image/avif'],
  ['.css', 'text/css; charset=utf-8'],
  ['.gif', 'image/gif'],
  ['.html', 'text/html; charset=utf-8'],
  ['.ico', 'image/x-icon'],
  ['.jpeg', 'image/jpeg'],
  ['.jpg', 'image/jpeg'],
  ['.js', 'text/javascript; charset=utf-8'],
  ['.json', 'application/json; charset=utf-8'],
  ['.mjs', 'text/javascript; charset=utf-8'],
  ['.mp3', 'audio/mpeg'],
  ['.mp4', 'video/mp4'],
  ['.ogg', 'audio/ogg'],
  ['.png', 'image/png'],
  ['.svg', 'image/svg+xml'],
  ['.ttf', 'font/ttf'],
  ['.txt', 'text/plain; charset=utf-8'],
  ['.wasm', 'application/wasm'],
  ['.webm', 'video/webm'],
  ['.webp', 'image/webp'],
  ['.woff', 'font/woff'],
  ['.woff2', 'font/woff2'],
]);

export const VIEW_CSP = [
  "default-src 'self' data: blob:",
  "script-src 'self' 'unsafe-inline'",
  "style-src 'self' 'unsafe-inline'",
  "img-src 'self' data: blob:",
  "font-src 'self' data:",
  "connect-src 'self'",
  "frame-src 'self' data: blob:",
  "worker-src 'none'",
  "object-src 'none'",
  "base-uri 'self'",
  "form-action 'self'",
].join('; ');

function acceptedEntry(entry) {
  if (typeof entry !== 'string' || !entry || isAbsolute(entry)) return false;
  return !entry.replaceAll('\\', '/').split('/').includes('..');
}

function clampedInteger(value, fallback) {
  return Number.isInteger(value) ? Math.max(1, Math.min(16384, value)) : fallback;
}

async function readVersions() {
  try {
    const source = await fs.readFile(resolve(REPO_ROOT, 'src/core/Version.h'), 'utf8');
    return {
      version: source.match(/kPluginVersion\s*=\s*"([^"]+)"/)?.[1] || 'dev',
      bridgeVersion: source.match(/kBridgeProtocolVersion\s*=\s*"([^"]+)"/)?.[1] || 'dev',
    };
  } catch {
    return { version: 'dev', bridgeVersion: 'dev' };
  }
}

export async function loadViewDefinition(inputPath) {
  const viewDir = resolve(inputPath);
  const manifestPath = resolve(viewDir, 'manifest.json');
  let manifest;
  try {
    manifest = JSON.parse(await fs.readFile(manifestPath, 'utf8'));
  } catch (error) {
    throw new Error(`Cannot read ${manifestPath}: ${error.message}`);
  }
  if (!manifest || typeof manifest !== 'object' || Array.isArray(manifest)) {
    throw new Error(`${manifestPath} must contain a JSON object.`);
  }

  const viewName = basename(viewDir);
  const modId = basename(dirname(viewDir));
  if (!MOD_ID.test(modId) || modId.length > 64) {
    throw new Error(`"${modId}" is not a valid OSF UI mod folder (<author>.<modname>).`);
  }
  if (!VIEW_NAME.test(viewName) || viewName.length > 64) {
    throw new Error(`"${viewName}" is not a valid lowercase OSF UI view folder.`);
  }
  if (manifest.id !== viewName) {
    throw new Error(`manifest.json id "${manifest.id ?? ''}" must equal view folder "${viewName}".`);
  }
  const entry = manifest.entry ?? 'index.html';
  if (!acceptedEntry(entry)) {
    throw new Error(`manifest.json entry "${entry}" must remain inside the view folder.`);
  }
  const entryPath = resolve(viewDir, entry);
  if (!existsSync(entryPath)) {
    throw new Error(`View entry does not exist: ${entryPath}`);
  }
  const versions = await readVersions();
  return {
    viewDir,
    viewsRoot: resolve(viewDir, '..', '..'),
    manifestPath,
    fixturePath: resolve(viewDir, 'osfui.mock.json'),
    modId,
    viewName,
    qualifiedId: `${modId}/${viewName}`,
    entry: entry.replaceAll('\\', '/'),
    title: typeof manifest.title === 'string' ? manifest.title : `${modId}/${viewName}`,
    width: clampedInteger(manifest.width, 1600),
    height: clampedInteger(manifest.height, 900),
    transparent: manifest.transparent !== false,
    nativeBridge: manifest.permissions?.nativeBridge === true,
    ...versions,
  };
}

export function injectBootstrap(html) {
  const tag = '<script src="/__osfui/bootstrap.js"></script>';
  const head = /<head(?:\s[^>]*)?>/i;
  if (head.test(html)) return html.replace(head, (match) => `${match}\n  ${tag}`);
  const root = /<html(?:\s[^>]*)?>/i;
  if (root.test(html)) return html.replace(root, (match) => `${match}\n<head>${tag}</head>`);
  return `${tag}\n${html}`;
}

export function safeResolve(root, urlPath) {
  let decoded;
  try {
    decoded = decodeURIComponent(urlPath);
  } catch {
    return null;
  }
  const candidate = resolve(root, `.${decoded.replaceAll('/', sep)}`);
  const rel = relative(root, candidate);
  if (rel === '..' || rel.startsWith(`..${sep}`) || isAbsolute(rel)) return null;
  return candidate;
}

async function readFixture(definition) {
  try {
    const parsed = JSON.parse(await fs.readFile(definition.fixturePath, 'utf8'));
    if (!parsed || typeof parsed !== 'object' || Array.isArray(parsed)) {
      return { ...DEFAULT_FIXTURE, $error: 'osfui.mock.json must contain a JSON object.' };
    }
    return parsed;
  } catch (error) {
    if (error.code !== 'ENOENT') {
      return { ...DEFAULT_FIXTURE, $error: `Cannot parse osfui.mock.json: ${error.message}` };
    }
    return DEFAULT_FIXTURE;
  }
}

function jsonResponse(response, value, status = 200) {
  response.writeHead(status, {
    'Content-Type': 'application/json; charset=utf-8',
    'Cache-Control': 'no-store',
  });
  response.end(JSON.stringify(value, null, 2));
}

function textResponse(response, value, type, status = 200, headers = {}) {
  response.writeHead(status, {
    'Content-Type': type,
    'Cache-Control': 'no-store',
    ...headers,
  });
  response.end(value);
}

async function serveStatic(response, path, { injectHtml = false } = {}) {
  let stat;
  try {
    stat = await fs.stat(path);
  } catch {
    response.writeHead(404).end('Not found');
    return;
  }
  if (!stat.isFile()) {
    response.writeHead(404).end('Not found');
    return;
  }
  const extension = extname(path).toLowerCase();
  const headers = {
    'Cache-Control': 'no-store',
    'Content-Type': MIME.get(extension) || 'application/octet-stream',
    'Content-Length': stat.size,
  };
  if (injectHtml && extension === '.html') {
    const html = injectBootstrap(await fs.readFile(path, 'utf8'));
    delete headers['Content-Length'];
    headers['Content-Security-Policy'] = VIEW_CSP;
    response.writeHead(200, headers);
    response.end(html);
    return;
  }
  response.writeHead(200, headers);
  createReadStream(path).pipe(response);
}

function fallbackSharedPath(urlPath) {
  if (urlPath === '/shared/osfui.js') {
    return resolve(REPO_ROOT, 'frontend/src/shared-kit/osfui.js');
  }
  if (urlPath === '/shared/osfui.css') {
    return resolve(REPO_ROOT, 'frontend/src/shared-kit/osfui.css');
  }
  return null;
}

export async function createViewHarness(inputPath, options = {}) {
  let definition = await loadViewDefinition(inputPath);
  const clients = new Set();
  const host = options.host || '127.0.0.1';
  const requestedPort = Number.isInteger(options.port) ? options.port : 8081;

  const server = createServer(async (request, response) => {
    try {
      const url = new URL(request.url || '/', `http://${request.headers.host || host}`);
      const path = url.pathname;
      if (path === '/' || path === '/__osfui' || path === '/__osfui/') {
        textResponse(response, HARNESS_HTML, 'text/html; charset=utf-8');
        return;
      }
      if (path === '/__osfui/harness.css') {
        textResponse(response, HARNESS_CSS, 'text/css; charset=utf-8');
        return;
      }
      if (path === '/__osfui/harness.js') {
        textResponse(response, HARNESS_JS, 'text/javascript; charset=utf-8');
        return;
      }
      if (path === '/__osfui/bootstrap.js') {
        const publicMeta = publicDefinition(definition);
        const script = BOOTSTRAP_JS.replaceAll('__OSFUI_HARNESS_META__', JSON.stringify(publicMeta));
        textResponse(response, script, 'text/javascript; charset=utf-8');
        return;
      }
      if (path === '/__osfui/meta.json') {
        definition = await loadViewDefinition(definition.viewDir);
        jsonResponse(response, publicDefinition(definition));
        return;
      }
      if (path === '/__osfui/fixture.json') {
        jsonResponse(response, await readFixture(definition));
        return;
      }
      if (path === '/__osfui/events') {
        response.writeHead(200, {
          'Content-Type': 'text/event-stream',
          'Cache-Control': 'no-store',
          Connection: 'keep-alive',
        });
        response.write(': connected\n\n');
        clients.add(response);
        request.on('close', () => clients.delete(response));
        return;
      }

      let file = safeResolve(definition.viewsRoot, path);
      if (!file) {
        response.writeHead(403).end('Forbidden');
        return;
      }
      if (!existsSync(file)) {
        const fallback = fallbackSharedPath(path);
        if (fallback) file = fallback;
      }
      await serveStatic(response, file, { injectHtml: true });
    } catch (error) {
      jsonResponse(response, { error: error.message }, 500);
    }
  });

  await new Promise((resolveListen, reject) => {
    server.once('error', reject);
    server.listen(requestedPort, host, resolveListen);
  });
  const address = server.address();
  const port = typeof address === 'object' && address ? address.port : requestedPort;
  const url = `http://${host}:${port}/__osfui/`;

  let debounce;
  const watcher = watch(definition.viewDir, { recursive: true }, () => {
    clearTimeout(debounce);
    debounce = setTimeout(() => {
      for (const client of clients) client.write('event: reload\ndata: changed\n\n');
    }, 120);
  });
  const heartbeat = setInterval(() => {
    for (const client of clients) client.write(': heartbeat\n\n');
  }, 15000);

  return {
    server,
    url,
    definition,
    async close() {
      clearTimeout(debounce);
      clearInterval(heartbeat);
      watcher.close();
      for (const client of clients) client.end();
      clients.clear();
      await new Promise((resolveClose) => server.close(resolveClose));
    },
  };
}

export function publicDefinition(definition) {
  return {
    modId: definition.modId,
    viewName: definition.viewName,
    qualifiedId: definition.qualifiedId,
    title: definition.title,
    entry: definition.entry,
    width: definition.width,
    height: definition.height,
    transparent: definition.transparent,
    nativeBridge: definition.nativeBridge,
    version: definition.version,
    bridgeVersion: definition.bridgeVersion,
    viewUrl: `/${definition.qualifiedId}/${definition.entry}`,
  };
}
