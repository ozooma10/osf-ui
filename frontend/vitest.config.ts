import { resolve } from 'node:path';
import { defineConfig } from 'vitest/config';

export default defineConfig({
  resolve: {
    alias: { '@sdk': resolve(import.meta.dirname, '../sdk/osfui.d.ts') },
  },
  test: {
    // `// @vitest-environment jsdom` pragma. Keeps the pure-logic suite fast.
    environment: 'node',
    include: ['test/**/*.test.ts'],
    testTimeout: 15000,
  },
});
