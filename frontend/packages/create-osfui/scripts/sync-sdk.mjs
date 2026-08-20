import { copyFile, mkdir, readFile } from 'node:fs/promises';
import { dirname, resolve } from 'node:path';
import { fileURLToPath } from 'node:url';

const here = dirname(fileURLToPath(import.meta.url));
const packageRoot = resolve(here, '..');
const check = process.argv.slice(2).includes('--check');
const unknown = process.argv.slice(2).filter((arg) => arg !== '--check');
if (unknown.length > 0) throw new Error(`unknown argument: ${unknown.join(' ')}`);
const repositoryRoot = resolve(packageRoot, '..', '..', '..');
const papyrusApiFiles = ['OSFUI.psc', 'OSFUI_Settings.psc', 'OSFUI_View.psc'];

const files = [
  [resolve(repositoryRoot, 'sdk', 'OSFUI_API.h'), resolve(packageRoot, 'templates', 'native', 'OSFUI_API.h')],
  [resolve(repositoryRoot, 'sdk', 'OSFUI_JSON.h'), resolve(packageRoot, 'templates', 'native', 'OSFUI_JSON.h')],
  ...papyrusApiFiles.map((name) => [
    resolve(repositoryRoot, 'data', 'Scripts', 'Source', name),
    resolve(packageRoot, 'templates', 'papyrus', name),
  ]),
];

if (check) {
  const drifted = [];
  for (const [source, template] of files) {
    try {
      const [expected, actual] = await Promise.all([readFile(source), readFile(template)]);
      if (!expected.equals(actual)) drifted.push(template);
    } catch {
      drifted.push(template);
    }
  }
  if (drifted.length > 0) {
    throw new Error(
      `scaffolder SDK templates are stale:\n${drifted.map((file) => `  ${file}`).join('\n')}\n` +
        'Run from frontend/packages/create-osfui: npm run sync:sdk',
    );
  }
} else {
  for (const [source, template] of files) {
    await mkdir(dirname(template), { recursive: true });
    await copyFile(source, template);
  }
}
