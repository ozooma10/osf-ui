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
//   3. /osf.animation/*: maps to the sibling repo's real view directory, so
//      the shipped '../..' asset root resolves sibling-mod icons exactly as
//      it does in game, and devpages/osf.html can iframe the real view.
//   4. /osf.html: the OSF Animation self-mock preview page (devpages/).

import { existsSync } from 'node:fs';
import { readFile } from 'node:fs/promises';
import { dirname, extname, normalize, resolve } from 'node:path';
import { fileURLToPath } from 'node:url';
import type { Plugin } from 'vite';

const FRONTEND = resolve(dirname(fileURLToPath(import.meta.url)), '..');
const PADNAV = resolve(FRONTEND, 'src/legacy/padnav.js');
const OSF_PAGE = resolve(FRONTEND, 'devpages/osf.html');
/** The sibling repo's view dir; absent checkouts just 404 (osf.html explains). */
const OSF_ANIMATION = resolve(FRONTEND, '../..', 'OSF Animation/views/osf.animation');

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
        if (path === '/osf.html') {
          await sendFile(OSF_PAGE, TYPES['.html']!);
          return;
        }
        if (path.startsWith('/osf.animation/')) {
          const rest = normalize(path.slice('/osf.animation/'.length));
          // Containment: this middleware serves from outside the Vite root.
          if (rest.split(/[\\/]/).includes('..')) {
            response.statusCode = 403;
            response.end();
            return;
          }
          const file = resolve(OSF_ANIMATION, rest);
          if (existsSync(file)) {
            await sendFile(file, TYPES[extname(file).toLowerCase()] || 'application/octet-stream');
            return;
          }
        }
        next();
      });
    },
  };
}
