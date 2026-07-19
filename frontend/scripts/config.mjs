// config.mjs — paths and the output manifest, shared by build.mjs and
// verify-output.mjs. Kept separate so verification can import it without
// importing the builder (which would be a cycle, and a cycle across a
// top-level await deadlocks silently rather than erroring).

import { existsSync, readdirSync, statSync } from 'node:fs';
import { dirname, join, relative, resolve } from 'node:path';
import { fileURLToPath } from 'node:url';

const HERE = dirname(fileURLToPath(import.meta.url));
export const FRONTEND = resolve(HERE, '..');
export const REPO = resolve(FRONTEND, '..');

// `check:dist` builds into a scratch directory and byte-compares, so the
// staleness gate works whether or not the output has been committed yet.
export const OUT = process.env.OSFUI_VIEWS_OUT
  ? resolve(process.env.OSFUI_VIEWS_OUT)
  : join(REPO, 'data', 'OSFUI', 'views');

export const COMMITTED_OUT = join(REPO, 'data', 'OSFUI', 'views');

// Per-view emit mode. 'verbatim' ships the pre-migration hand-written main.js
// untouched; 'bundle' builds main.tsx through Vite. Flipping one view at a time
// is what keeps each migration phase independently revertable.
export const VIEWS = [
  { mod: 'osfui', name: 'keybinds', mode: 'bundle' },
  { mod: 'osfui', name: 'settings', mode: 'bundle' },
];

// Every file this build owns. verify-output asserts the emitted set is exactly
// this - so a stray file (a .map, a fixture, an orphaned chunk) fails the build
// rather than silently shipping in the next archive.
export function expectedOutputs() {
  const files = ['shared/osfui.js', 'shared/osfui.css', 'osfui/padnav.js'];
  for (const v of VIEWS) {
    files.push(
      `${v.mod}/${v.name}/index.html`,
      `${v.mod}/${v.name}/manifest.json`,
      `${v.mod}/${v.name}/main.js`,
      `${v.mod}/${v.name}/style.css`,
    );
  }
  return files.sort();
}

export function walk(dir, base = dir, acc = []) {
  if (!existsSync(dir)) return acc;
  for (const name of readdirSync(dir)) {
    const p = join(dir, name);
    if (statSync(p).isDirectory()) walk(p, base, acc);
    else acc.push(relative(base, p).split('\\').join('/'));
  }
  return acc;
}
