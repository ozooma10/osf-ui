// The built-in views as an @osfui/cli project: `npm run dev` serves them
// through the same authoring harness third-party mods
// use. Dev only — the shipped artifact is still produced by scripts/build.mjs
// (verbatim index.html/manifest, per-view IIFE), which this file never
// touches.
//
// The per-view manifest.json stays the artifact source of truth; the dev
// server derives its entries from those documents too.

import { defineConfig } from '@osfui/cli';

import { aliases } from './aliases.mjs';
import { builtinDevPlugin } from './scripts/builtin-dev-plugin';
import { VIEWS } from './scripts/config.mjs';

export default defineConfig({
  modId: 'osfui',
  // Order matters: views[0] is the view `osfui dev` opens first.
  views: VIEWS
    .map(({ name: id, manifest }) => ({
      id,
      title: manifest.title,
      description: manifest.description,
      kind: manifest.kind,
      hub: manifest.hub,
      debugOnly: manifest.debugOnly,
      pausesGame: manifest.pausesGame,
      openOnStart: manifest.openOnStart,
    }))
    .sort((a, b) => (a.id === 'settings' ? -1 : b.id === 'settings' ? 1 : a.id.localeCompare(b.id))),
  mock: 'osfui.mock.ts',
  // Dev-server-only extension: the aliases the views' sources use, plus the
  // legacy-contract shims (classic main.js -> module main.tsx and padnav).
  vite: {
    resolve: {
      alias: aliases,
    },
    plugins: [builtinDevPlugin()],
  },
});
