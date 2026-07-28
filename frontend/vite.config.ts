import { defineConfig } from 'vite';
import preact from '@preact/preset-vite';
import { resolve, dirname } from 'node:path';
import { fileURLToPath } from 'node:url';

// This config is ESM ("type": "module"), so __dirname does not exist.
const __dirname = dirname(fileURLToPath(import.meta.url));

// Production build config, consumed by scripts/build.mjs as its `configFile`
// (which overrides `build.rollupOptions` per view - Rollup cannot emit IIFE
// for a multi-input build). The dev loop does not run through this file:
// `osfui dev` assembles its own server config from osfui.config.ts.
export default defineConfig({
  plugins: [preact()],
  resolve: {
    alias: {
      '@lib': resolve(__dirname, 'src/lib'),
      '@ui': resolve(__dirname, 'src/ui'),
      '@views': resolve(__dirname, 'src/views'),
      '@devmock': resolve(__dirname, 'devmock'),
    },
  },
  build: {
    target: 'es2020',
    // Vite 8 is rolldown-based: 'esbuild' is deprecated there and needs a
    // separate esbuild install. `true` selects the built-in Oxc minifier,
    // whose output is pinned by the locked vite version — which is what the
    // deterministic build-output gates require.
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
});
