import { defineConfig } from 'vitest/config';
import preact from '@preact/preset-vite';
import { aliases } from './aliases.mjs';

export default defineConfig({
  plugins: [preact()],
  resolve: {
    alias: aliases,
  },
  test: {
    // `// @vitest-environment jsdom` pragma. Keeps the pure-logic suite fast.
    environment: 'node',
    include: ['test/**/*.test.{ts,tsx}'],
    testTimeout: 15000,
  },
});
