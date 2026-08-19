import { defineConfig } from 'vite';
import preact from '@preact/preset-vite';
import { aliases } from './aliases.mjs';

export default defineConfig({
  plugins: [preact()],
  resolve: {
    alias: aliases,
  },
  build: {
    target: 'es2020',
    minify: true,
    // One stable CSS file per view.
    cssCodeSplit: false,
    sourcemap: false,
    modulePreload: false,
    assetsInlineLimit: 0,
    reportCompressedSize: false,
  },
});
