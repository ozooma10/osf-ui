import { defineConfig } from 'vitest/config';
import preact from '@preact/preset-vite';
import { aliases } from './aliases.mjs';

export default defineConfig({
  plugins: [preact()],
  resolve: {
    alias: aliases,
  },
  test: {
    // Node by default; component tests opt into jsdom with a per-file
    // `// @vitest-environment jsdom` pragma. Keeps the pure-logic suite fast.
    environment: 'node',
    include: ['test/**/*.test.{ts,tsx}'],
    // Build-output gates read build/frontend/views, which `npm run build` must have
    // produced first. `npm run verify` sequences them correctly.
    testTimeout: 15000,
  },
});
