import { readFile } from 'node:fs/promises';
import { basename, dirname, resolve } from 'node:path';
import { fileURLToPath } from 'node:url';

import { normalizePath } from 'vite';

import { HARNESS_CSS, HARNESS_HTML } from './harness-assets.mjs';
import { BRIDGE_VERSION, HOST_VERSION } from './constants.mjs';
import { readSharedAsset } from './shared-assets.mjs';

const HERE = dirname(fileURLToPath(import.meta.url));
const SHARED_PREFIX = '\0osfui-shared:';
/** URL the mock module is importable at; resolveId maps it to the real file. */
const MOCK_ENTRY = '/__osfui/mock-entry.js';
/** src/browser modules served verbatim at /__osfui/<name>. */
const BROWSER_MODULES = new Set([
  '/__osfui/stage-fit.js',
  '/__osfui/tools-model.js',
  '/__osfui/traffic-model.js',
  '/__osfui/pseudo.js',
  '/__osfui/mock-loader.js',
  '/__osfui/mock-runtime.js',
]);
const CSP = [
  "default-src 'self' data: blob:",
  "script-src 'self' 'unsafe-inline'",
  "style-src 'self' 'unsafe-inline'",
  "img-src 'self' data: blob:",
  "font-src 'self' data:",
  "connect-src 'self' ws: wss:",
  "frame-src 'self' data: blob:",
  "worker-src 'none'",
  "object-src 'none'",
  "base-uri 'self'",
].join('; ');
/** The shell page has no inline scripts, so it can be locked down harder. */
const SHELL_CSP = [
  "default-src 'self'",
  "script-src 'self'",
  "style-src 'self' 'unsafe-inline'",
  "img-src 'self' data:",
  "connect-src 'self' ws: wss:",
  "frame-src 'self'",
  "object-src 'none'",
  "base-uri 'self'",
].join('; ');

function send(response, body, type) {
  response.statusCode = 200;
  response.setHeader('Content-Type', type);
  response.setHeader('Cache-Control', 'no-store');
  response.end(body);
}

/** A file from src/browser/, shipped with the package. */
function browserAsset(name) {
  return readFile(resolve(HERE, 'browser', name), 'utf8');
}

/** Match the runtime's declared-target selection for the 1.x helper facade. */
export function isPre2Target(targetVersion) {
  const match = /^(\d+)(?:\.\d+)?(?:\.\d+)?$/.exec(targetVersion || '');
  if (match === null) return false;
  const parts = String(targetVersion).split('.').map((part) => BigInt(part));
  return parts.every((part) => part <= 0xffffffffn) && parts[0] < 2n;
}

export function harnessPlugin(project, selectedView) {
  const metaFor = (view) => {
    const legacyApi = isPre2Target(view.targetVersion);
    const path = `/${project.modId}/${view.id}/${view.entry}`;
    return {
      modId: project.modId,
      viewName: view.id,
      qualifiedId: view.qualifiedId,
      title: view.title,
      width: view.width,
      height: view.height,
      transparent: view.transparent,
      nativeBridge: view.permissions.nativeBridge,
      targetVersion: view.targetVersion || '',
      legacyApi,
      viewUrl: legacyApi ? `${path}?osfui-api=1` : path,
      version: HOST_VERSION,
      bridgeVersion: BRIDGE_VERSION,
      // Absent when the project has no mock; mock-loader.js skips the import.
      ...(project.mockPath ? { mockUrl: MOCK_ENTRY, mockName: basename(project.mockPath) } : {}),
    };
  };
  /** Which project view a served page belongs to, by URL prefix. */
  const viewForPath = (path) =>
    project.views.find((view) => path.startsWith(`/${project.modId}/${view.id}/`)) || selectedView;
  return {
    name: 'osfui-author-harness',
    enforce: 'pre',
    resolveId(source) {
      if (source === '/shared/osfui.js') return `${SHARED_PREFIX}osfui.js`;
      if (source === '/shared/osfui.css') return `${SHARED_PREFIX}osfui.css`;
      const bare = source.split('?')[0];
      // The project's mock, wherever it lives, importable at a stable URL and
      // transformed by Vite (TypeScript, aliases, JSON default export).
      if (bare === MOCK_ENTRY && project.mockPath) return normalizePath(project.mockPath);
      // The loader is injected into view pages as a module script, so Vite's
      // import analysis must be able to resolve it (the middleware alone
      // covers the request but not the warmup, which logs a pre-transform
      // error). Resolving to the real file also routes its ./mock-runtime.js
      // (and transitively ./tools-model.js, ./pseudo.js) imports through
      // Vite's graph.
      if (bare === '/__osfui/mock-loader.js') {
        return normalizePath(resolve(HERE, 'browser', 'mock-loader.js'));
      }
    },
    async load(id) {
      if (!id.startsWith(SHARED_PREFIX)) return null;
      return readSharedAsset(id.slice(SHARED_PREFIX.length));
    },
    transformIndexHtml: {
      order: 'pre',
      handler(html, context) {
        if (context.path.startsWith('/__osfui/')) return html;
        // Order matters: the inline meta names the page's view, the classic
        // bootstrap installs the queuing bridge stub at parse time, and the
        // module loader then brings up the mock before any module view entry
        // runs (module scripts execute in order and honor top-level await).
        return [
          {
            tag: 'script',
            children: `window.__OSFUI_HARNESS_META__=${JSON.stringify(metaFor(viewForPath(context.path)))};`,
            injectTo: 'head-prepend',
          },
          {
            tag: 'script',
            attrs: { src: '/__osfui/bootstrap.js' },
            injectTo: 'head-prepend',
          },
          {
            tag: 'script',
            attrs: { type: 'module', src: '/__osfui/mock-loader.js' },
            injectTo: 'head-prepend',
          },
        ];
      },
    },
    configureServer(server) {
      if (project.mockPath) {
        server.watcher.add(project.mockPath);
        server.watcher.on('change', (path) => {
          if (path === project.mockPath) server.ws.send({ type: 'full-reload' });
        });
      }
      server.middlewares.use(async (request, response, next) => {
        const url = new URL(request.url || '/', 'http://osfui.local');
        if (url.pathname === '/shared/osfui.js') {
          send(response, await readSharedAsset('osfui.js'), 'text/javascript; charset=utf-8');
          return;
        }
        if (url.pathname === '/shared/osfui.css') {
          send(response, await readSharedAsset('osfui.css'), 'text/css; charset=utf-8');
          return;
        }
        if (!url.pathname.startsWith('/__osfui/')) {
          response.setHeader('Content-Security-Policy', CSP);
          next();
          return;
        }
        response.setHeader('Content-Security-Policy', SHELL_CSP);
        if (url.pathname === '/__osfui/' || url.pathname === '/__osfui/index.html') {
          send(response, HARNESS_HTML, 'text/html; charset=utf-8');
        } else if (url.pathname === '/__osfui/harness.css') {
          send(response, HARNESS_CSS, 'text/css; charset=utf-8');
        } else if (url.pathname === '/__osfui/harness.js') {
          send(response, await browserAsset('shell.js'), 'text/javascript; charset=utf-8');
        } else if (BROWSER_MODULES.has(url.pathname)) {
          // Every module under src/browser/ that the shell or the mock
          // runtime imports by /__osfui/ URL. One set, so a new import can't
          // silently 404 (the toolchain test walks the import closure).
          send(
            response,
            await browserAsset(url.pathname.slice('/__osfui/'.length)),
            'text/javascript; charset=utf-8',
          );
        } else if (url.pathname === '/__osfui/bootstrap.js') {
          send(response, await browserAsset('bootstrap.js'), 'text/javascript; charset=utf-8');
        } else if (url.pathname === '/__osfui/meta.json') {
          send(
            response,
            JSON.stringify({
              views: project.views.map(metaFor),
              initial: selectedView.qualifiedId,
            }),
            'application/json; charset=utf-8',
          );
        } else {
          next();
        }
      });
    },
  };
}
