// The built-in views as an @osfui/cli project: `osfui dev` (or `npm run
// dev:cli`) serves them through the same authoring harness third-party mods
// use. Dev only — the shipped artifact is still produced by scripts/build.mjs
// (verbatim index.html/manifest, per-view IIFE), which this file never
// touches.
//
// The per-view manifest.json stays the artifact source of truth; the entries
// here mirror it for the dev server (test/devconfig.test.ts pins the parity).

import { dirname, resolve } from 'node:path';
import { fileURLToPath } from 'node:url';

import { defineConfig } from '@osfui/cli';

import { builtinDevPlugin } from './scripts/builtin-dev-plugin';

const frontend = dirname(fileURLToPath(import.meta.url));

export default defineConfig({
  modId: 'osfui',
  views: [
    // Order matters: views[0] is the surface `osfui dev` opens first.
    {
      id: 'settings',
      title: 'Mods',
      description: 'Installed mods - settings, terminals and HUD toggles.',
      hub: false,
    },
    {
      id: 'keybinds',
      title: 'Keybinds',
      description: 'Full keyboard map of mod and Game Bindings',
    },
    {
      id: 'benchmark',
      title: 'Web Performance Lab',
      description: 'Repeatable browser rendering workloads and frame-time reference measurements.',
      kind: 'menu',
      debugOnly: true,
      pausesGame: false,
    },
    {
      id: 'handoff',
      title: 'Local Interface Link',
      description: 'Warm platform surface for first-load view handoffs',
      kind: 'menu',
      hub: false,
      openOnStart: false,
    },
  ],
  mock: 'osfui.mock.ts',
  // Dev-server-only extension: the aliases the views' sources use, plus the
  // legacy-contract shims (classic main.js -> module main.tsx, padnav, the
  // OSF Animation sibling-repo mappings).
  vite: {
    resolve: {
      alias: {
        '@lib': resolve(frontend, 'src/lib'),
        '@ui': resolve(frontend, 'src/ui'),
        '@views': resolve(frontend, 'src/views'),
        '@devmock': resolve(frontend, 'devmock'),
      },
    },
    plugins: [builtinDevPlugin()],
  },
});
