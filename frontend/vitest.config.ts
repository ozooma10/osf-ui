import { defineConfig } from 'vitest/config';
import preact from '@preact/preset-vite';
import { resolve, dirname } from 'node:path';
import { fileURLToPath } from 'node:url';

const __dirname = dirname(fileURLToPath(import.meta.url));

export default defineConfig({
  plugins: [preact()],
  resolve: {
    alias: {
      '@sdk': resolve(__dirname, '../sdk/osfui.d.ts'),
      '@lib': resolve(__dirname, 'src/lib'),
      '@ui': resolve(__dirname, 'src/ui'),
      '@views': resolve(__dirname, 'src/views'),
      '@harness': resolve(__dirname, 'harness'),
    },
  },
  test: {
    // Node by default; component tests opt into jsdom with a per-file
    // `// @vitest-environment jsdom` pragma. Keeps the pure-logic suite fast.
    environment: 'node',
    include: ['test/**/*.test.{ts,tsx}'],
    // Build-output gates read data/OSFUI/views, which `npm run build` must have
    // produced first. `npm run verify` sequences them correctly.
    testTimeout: 15000,
  },
});
