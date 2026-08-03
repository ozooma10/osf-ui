import { cp, mkdir, readFile } from 'node:fs/promises';
import { dirname, resolve } from 'node:path';
import { fileURLToPath } from 'node:url';

const here = dirname(fileURLToPath(import.meta.url));
const packageRoot = resolve(here, '..');
const repoRoot = resolve(packageRoot, '..', '..');

const copies = [
  ['frontend/src/shared-kit/osfui.js', 'assets/osfui.js'],
  ['frontend/src/shared-kit/osfui.css', 'assets/osfui.css'],
  ['sdk/osfui.d.ts', 'types/osfui.d.ts'],
];

if (process.argv.includes('--check')) {
  const stale = [];
  for (const [source, target] of copies) {
    const [sourceBytes, targetBytes] = await Promise.all([
      readFile(resolve(repoRoot, source)),
      readFile(resolve(packageRoot, target)).catch(() => null),
    ]);
    if (!targetBytes || !sourceBytes.equals(targetBytes)) stale.push(target);
  }
  if (stale.length) {
    throw new Error(`Packaged public assets are stale: ${stale.join(', ')}. Run npm install.`);
  }
} else {
  await mkdir(resolve(packageRoot, 'assets'), { recursive: true });
  await mkdir(resolve(packageRoot, 'types'), { recursive: true });
  for (const [source, target] of copies) {
    await cp(resolve(repoRoot, source), resolve(packageRoot, target));
  }
}
