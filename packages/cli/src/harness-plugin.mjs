import { readFile } from 'node:fs/promises';
import { dirname, resolve } from 'node:path';
import { fileURLToPath } from 'node:url';

import {
  BOOTSTRAP_JS,
  HARNESS_CSS,
  HARNESS_HTML,
  HARNESS_JS,
} from './harness-assets.mjs';
import { BRIDGE_VERSION, HOST_VERSION } from './constants.mjs';

const HERE = dirname(fileURLToPath(import.meta.url));
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

function send(response, body, type) {
  response.statusCode = 200;
  response.setHeader('Content-Type', type);
  response.setHeader('Cache-Control', 'no-store');
  response.end(body);
}

async function sharedAsset(name) {
  for (const path of [
    resolve(HERE, '..', 'assets', name),
    resolve(HERE, '..', '..', '..', 'frontend', 'src', 'shared-kit', name),
  ]) {
    try { return await readFile(path, 'utf8'); } catch {}
  }
  throw new Error(`Missing packaged shared asset ${name}.`);
}

export function harnessPlugin(project, selectedView) {
  const view = selectedView;
  const meta = {
    modId: project.modId,
    viewName: view.id,
    qualifiedId: view.qualifiedId,
    title: view.title,
    width: view.width,
    height: view.height,
    transparent: view.transparent,
    nativeBridge: view.permissions.nativeBridge,
    viewUrl: `/${project.modId}/${view.id}/${view.entry}`,
    version: HOST_VERSION,
    bridgeVersion: BRIDGE_VERSION,
  };
  return {
    name: 'osfui-author-harness',
    enforce: 'pre',
    transformIndexHtml: {
      order: 'pre',
      handler(html, context) {
        if (context.path.startsWith('/__osfui/')) return html;
        return [{
          tag: 'script',
          attrs: { src: '/__osfui/bootstrap.js' },
          injectTo: 'head-prepend',
        }];
      },
    },
    configureServer(server) {
      server.watcher.add(project.mockPath);
      server.watcher.on('change', (path) => {
        if (path === project.mockPath) server.ws.send({ type: 'full-reload' });
      });
      server.middlewares.use(async (request, response, next) => {
        const url = new URL(request.url || '/', 'http://osfui.local');
        if (url.pathname === '/shared/osfui.js') {
          send(response, await sharedAsset('osfui.js'), 'text/javascript; charset=utf-8');
          return;
        }
        if (url.pathname === '/shared/osfui.css') {
          send(response, await sharedAsset('osfui.css'), 'text/css; charset=utf-8');
          return;
        }
        if (!url.pathname.startsWith('/__osfui/')) {
          response.setHeader('Content-Security-Policy', CSP);
          next();
          return;
        }
        if (url.pathname === '/__osfui/' || url.pathname === '/__osfui/index.html') {
          send(response, HARNESS_HTML, 'text/html; charset=utf-8');
        } else if (url.pathname === '/__osfui/harness.css') {
          send(response, HARNESS_CSS, 'text/css; charset=utf-8');
        } else if (url.pathname === '/__osfui/harness.js') {
          send(response, HARNESS_JS, 'text/javascript; charset=utf-8');
        } else if (url.pathname === '/__osfui/bootstrap.js') {
          send(
            response,
            `const __OSFUI_HARNESS_META__=${JSON.stringify(meta)};\n${BOOTSTRAP_JS}`,
            'text/javascript; charset=utf-8',
          );
        } else if (url.pathname === '/__osfui/meta.json') {
          send(response, JSON.stringify(meta), 'application/json; charset=utf-8');
        } else if (url.pathname === '/__osfui/fixture.json') {
          let fixture = '{"state":{},"requests":{},"locales":{}}';
          try { fixture = await readFile(project.mockPath, 'utf8'); } catch {}
          send(response, fixture, 'application/json; charset=utf-8');
        } else {
          next();
        }
      });
    },
  };
}

export { CSP as AUTHOR_CSP };
