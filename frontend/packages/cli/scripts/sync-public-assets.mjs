import { cp, mkdir, readFile, writeFile } from 'node:fs/promises';
import { dirname, resolve } from 'node:path';
import { fileURLToPath, pathToFileURL } from 'node:url';

const here = dirname(fileURLToPath(import.meta.url));
const packageRoot = resolve(here, '..');
const repoRoot = resolve(packageRoot, '..', '..', '..');
const { composeHelper } = await import(pathToFileURL(
  resolve(repoRoot, 'frontend/scripts/compose-helper.mjs'),
).href);

const copies = [
  ['frontend/src/shared-kit/osfui.css', 'assets/osfui.css'],
  ['frontend/src/legacy/padnav.js', 'assets/gamepadnav.js'],
  ['sdk/osfui.d.ts', 'types/osfui.d.ts'],
];

if (process.argv.includes('--check')) {
  const stale = [];
  const helperBytes = await readFile(resolve(packageRoot, 'assets/osfui.js')).catch(() => null);
  if (!helperBytes || helperBytes.toString('utf8') !== composeHelper()) stale.push('assets/osfui.js');
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
  await writeFile(resolve(packageRoot, 'assets/osfui.js'), composeHelper(), 'utf8');
  for (const [source, target] of copies) {
    await cp(resolve(repoRoot, source), resolve(packageRoot, target));
  }
}
