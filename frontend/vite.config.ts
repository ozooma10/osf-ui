import { defineConfig } from 'vite';
import preact from '@preact/preset-vite';
import { resolve, dirname } from 'node:path';
import { fileURLToPath } from 'node:url';

// This config is ESM ("type": "module"), so __dirname does not exist.
const __dirname = dirname(fileURLToPath(import.meta.url));

// Shared defaults. Production output shape (IIFE, stable filenames, per-view
// outDir) is driven by scripts/build.mjs, which overrides `build.rollupOptions`
// per view - Rollup cannot emit IIFE for a multi-input build.
export default defineConfig({
  root: resolve(__dirname, 'harness'),
  plugins: [preact()],
  resolve: {
    alias: {
      '@lib': resolve(__dirname, 'src/lib'),
      '@ui': resolve(__dirname, 'src/ui'),
      '@views': resolve(__dirname, 'src/views'),
      '@harness': resolve(__dirname, 'harness'),
    },
  },
  build: {
    target: 'es2020',
    // Vite 8 is rolldown-based: 'esbuild' is deprecated there and needs a
    // separate esbuild install. `true` selects the built-in Oxc minifier,
    // whose output is pinned by the locked vite version — which is what the
    // byte-compare staleness gate (check:dist) requires.
    minify: true,
    // One CSS file per view, in source order. keybinds/style.css in particular
    // has load-order-dependent cascade (its "Input Map overhaul" block
    // deliberately overrides the earlier rules), so splitting or reordering
    // would change appearance.
    cssCodeSplit: false,
    // A .map under data/ would ship in every archive - nothing in package.ps1
    // or CI excludes by extension. Dev source maps come from the dev server.
    sourcemap: false,
    modulePreload: false,
    assetsInlineLimit: 0,
    reportCompressedSize: false,
  },
  server: {
    port: 8080,
    fs: {
      // The harness reads two sibling-repo files (OSF Animation's UISettings.cpp
      // and this repo's src/core/Version.h) exactly as the old python-rooted
      // server did.
      allow: [resolve(__dirname, '..'), resolve(__dirname, '../..')],
    },
  },
});
