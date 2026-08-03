import { defineConfig } from 'vite';
import preact from '@preact/preset-vite';
import { aliases } from './aliases.mjs';

// Production build config, consumed by scripts/build.mjs as its `configFile`
// (which overrides `build.rollupOptions` per view - Rollup cannot emit IIFE
// for a multi-input build). The dev loop does not run through this file:
// `osfui dev` assembles its own server config from osfui.config.ts.
export default defineConfig({
  plugins: [preact()],
  resolve: {
    alias: aliases,
  },
  build: {
    target: 'es2020',
    // Vite 8 is rolldown-based: 'esbuild' is deprecated there and needs a
    // separate esbuild install. `true` selects the built-in Oxc minifier,
    // whose output is pinned by the locked vite version — which is what the
    // deterministic build-output gates require.
    minify: true,
    // One stable CSS file per view.
    cssCodeSplit: false,
    // A .map under data/ would ship in every archive - nothing in package.ps1
    // or CI excludes by extension. Dev source maps come from the dev server.
    sourcemap: false,
    modulePreload: false,
    assetsInlineLimit: 0,
    reportCompressedSize: false,
  },
});
