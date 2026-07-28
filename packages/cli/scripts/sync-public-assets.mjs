import { cp, mkdir } from 'node:fs/promises';
import { dirname, resolve } from 'node:path';
import { fileURLToPath } from 'node:url';

const here = dirname(fileURLToPath(import.meta.url));
const packageRoot = resolve(here, '..');
const repoRoot = resolve(packageRoot, '..', '..');

await mkdir(resolve(packageRoot, 'assets'), { recursive: true });
await mkdir(resolve(packageRoot, 'types'), { recursive: true });
await cp(resolve(repoRoot, 'frontend/src/shared-kit/osfui.js'),
  resolve(packageRoot, 'assets/osfui.js'));
await cp(resolve(repoRoot, 'frontend/src/shared-kit/osfui.css'),
  resolve(packageRoot, 'assets/osfui.css'));
await cp(resolve(repoRoot, 'sdk/osfui.d.ts'),
  resolve(packageRoot, 'types/osfui.d.ts'));
