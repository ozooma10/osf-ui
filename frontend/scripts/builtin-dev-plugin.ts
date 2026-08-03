// Frontend-local Vite plugin for `osfui dev` (wired in by osfui.config.ts).
// Dev only — the shipped artifact is produced by scripts/build.mjs and never
// passes through this file.
//
// It bridges the gap between the built-ins' legacy classic-script contract
// and a Vite dev server rooted at src/views:
//
//   1. index.html rewrite: shipped pages load the built IIFE by its artifact
//      name (<script src="main.js">, gated classic by verify-output.mjs); in
//      dev the source is the module entry, so the tag becomes
//      <script type="module" src="./main.tsx">.
//   2. /osfui/padnav.js: the pages reference ../padnav.js, which only exists
//      at that path in the built output (copied from src/legacy/padnav.js).

import { readFile } from 'node:fs/promises';
import { dirname, resolve } from 'node:path';
import { fileURLToPath } from 'node:url';
import type { Plugin } from 'vite';

const FRONTEND = resolve(dirname(fileURLToPath(import.meta.url)), '..');
const PADNAV = resolve(FRONTEND, 'src/legacy/padnav.js');

const TYPES: Record<string, string> = {
  '.html': 'text/html; charset=utf-8',
  '.js': 'text/javascript; charset=utf-8',
  '.css': 'text/css; charset=utf-8',
  '.json': 'application/json; charset=utf-8',
  '.svg': 'image/svg+xml',
  '.png': 'image/png',
  '.webp': 'image/webp',
  '.jpg': 'image/jpeg',
  '.woff2': 'font/woff2',
};

export function builtinDevPlugin(): Plugin {
  return {
    name: 'osfui-builtin-dev',
    transformIndexHtml: {
      order: 'pre',
      handler(html, context) {
        if (context.path.startsWith('/__osfui/')) return html;
        return html.replace(
          '<script src="main.js"></script>',
          '<script type="module" src="./main.tsx"></script>',
        );
      },
    },
    configureServer(server) {
      server.watcher.add(PADNAV);
      server.watcher.on('change', (path) => {
        if (path === PADNAV) server.ws.send({ type: 'full-reload' });
      });
      server.middlewares.use(async (request, response, next) => {
        const url = new URL(request.url || '/', 'http://osfui.local');
        const path = decodeURIComponent(url.pathname);
        const sendFile = async (file: string, type: string) => {
          try {
            const body = await readFile(file);
            response.statusCode = 200;
            response.setHeader('Content-Type', type);
            response.setHeader('Cache-Control', 'no-store');
            response.end(body);
          } catch {
            next();
          }
        };
        if (path === '/osfui/padnav.js') {
          await sendFile(PADNAV, TYPES['.js']!);
          return;
        }
        next();
      });
    },
  };
}
