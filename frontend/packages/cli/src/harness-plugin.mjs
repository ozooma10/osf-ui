import { readFile } from 'node:fs/promises';
import { basename, dirname, resolve } from 'node:path';
import { fileURLToPath } from 'node:url';

import { normalizePath } from 'vite';

import { BRIDGE_VERSION, OSFUI_RELEASE_VERSION } from './constants.mjs';
import { HARNESS_CSS, HARNESS_HTML } from './harness-assets.mjs';
import { readSharedAsset } from './shared-assets.mjs';

const HERE = dirname(fileURLToPath(import.meta.url));
const MOCK_ENTRY = '/__osfui/mock-entry.js';
const SHARED_PREFIX = '\0osfui-shared:';
const BROWSER_MODULES = new Set(['mock-loader.js', 'mock-runtime.js', 'tools-model.js']);
const CSP = [
  "default-src 'self' data: blob:",
  "script-src 'self' 'unsafe-inline'",
  "style-src 'self' 'unsafe-inline'",
  "img-src 'self' data: blob:",
  "connect-src 'self' ws: wss:",
  "worker-src 'none'",
  "object-src 'none'",
].join('; ');

function send(response, body, type) {
  response.statusCode = 200;
  response.setHeader('Content-Type', type);
  response.setHeader('Cache-Control', 'no-store');
  response.end(body);
}

function browserAsset(name) {
  return readFile(resolve(HERE, 'browser', name), 'utf8');
}

export function harnessPlugin(project, selectedView) {
  const metaFor = (view) => ({
    modId: project.modId,
    viewName: view.id,
    qualifiedId: view.qualifiedId,
    title: view.title,
    width: view.width,
    height: view.height,
    transparent: view.transparent,
    nativeBridge: true,
    targetVersion: view.targetVersion || '',
    viewUrl: `/${project.modId}/${view.id}/${view.entry}`,
    version: OSFUI_RELEASE_VERSION,
    bridgeVersion: BRIDGE_VERSION,
    ...(project.mockPath ? { mockUrl: MOCK_ENTRY, mockName: basename(project.mockPath) } : {}),
  });
  const viewForPath = (path) =>
    project.views.find((view) => path.startsWith(`/${project.modId}/${view.id}/`)) || selectedView;

  return {
    name: 'osfui-authoring-harness',
    enforce: 'pre',
    resolveId(source) {
      if (source === '/shared/osfui.js') return `${SHARED_PREFIX}osfui.js`;
      if (source === '/shared/osfui.css') return `${SHARED_PREFIX}osfui.css`;
      if (source === '/shared/gamepadnav.js') return `${SHARED_PREFIX}gamepadnav.js`;
      const bare = source.split('?')[0];
      if (bare === MOCK_ENTRY && project.mockPath) return normalizePath(project.mockPath);
    },
    async load(id) {
      if (!id.startsWith(SHARED_PREFIX)) return null;
      return readSharedAsset(id.slice(SHARED_PREFIX.length));
    },
    transformIndexHtml: {
      order: 'pre',
      handler(html, context) {
        if (context.path.startsWith('/__osfui/')) return html;
        return {
          html,
          tags: [{
            tag: 'script',
            children: `window.__OSFUI_HARNESS_META__=${JSON.stringify(metaFor(viewForPath(context.path)))};`,
            injectTo: 'head-prepend',
          },
          { tag: 'script', attrs: { src: '/__osfui/bootstrap.js' }, injectTo: 'head-prepend' },
          { tag: 'script', attrs: { type: 'module', src: '/__osfui/mock-loader.js' }, injectTo: 'head-prepend' },
          ],
        };
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
        if (url.pathname === '/shared/gamepadnav.js') {
          send(response, await readSharedAsset('gamepadnav.js'), 'text/javascript; charset=utf-8');
          return;
        }
        response.setHeader('Content-Security-Policy', CSP);
        if (url.pathname === '/__osfui/' || url.pathname === '/__osfui/index.html') {
          send(response, HARNESS_HTML, 'text/html; charset=utf-8');
        } else if (url.pathname === '/__osfui/harness.css') {
          send(response, HARNESS_CSS, 'text/css; charset=utf-8');
        } else if (url.pathname === '/__osfui/harness.js') {
          send(response, await browserAsset('shell.js'), 'text/javascript; charset=utf-8');
        } else if (url.pathname === '/__osfui/bootstrap.js') {
          send(response, await browserAsset('bootstrap.js'), 'text/javascript; charset=utf-8');
        } else if (url.pathname.startsWith('/__osfui/') &&
                   BROWSER_MODULES.has(url.pathname.slice('/__osfui/'.length))) {
          send(
            response,
            await browserAsset(url.pathname.slice('/__osfui/'.length)),
            'text/javascript; charset=utf-8',
          );
        } else if (url.pathname === '/__osfui/meta.json') {
          send(response, JSON.stringify({
            views: project.views.map(metaFor),
            initial: selectedView.qualifiedId,
          }), 'application/json; charset=utf-8');
        } else {
          next();
        }
      });
    },
  };
}
