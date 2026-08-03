import { readdir } from 'node:fs/promises';
import { spawnSync } from 'node:child_process';
import { dirname, resolve } from 'node:path';
import { fileURLToPath } from 'node:url';

const packageRoot = resolve(dirname(fileURLToPath(import.meta.url)), '..');
const tests = (await readdir(resolve(packageRoot, 'test')))
  .filter((name) => name.endsWith('.test.mjs'))
  .sort();

for (const test of tests) {
  const result = spawnSync(process.execPath, ['--test', resolve(packageRoot, 'test', test)], {
    stdio: 'inherit',
  });
  if (result.error) throw result.error;
  if (result.status !== 0) process.exit(result.status ?? 1);
}
