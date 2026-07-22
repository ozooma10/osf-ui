// Paths and the output manifest, shared by build.mjs and verify-output.mjs.
// Separate so verification can import it without importing the builder: that
// cycle would cross a top-level await and deadlock silently instead of erroring.

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

// Per-view emit mode: 'verbatim' copies a hand-written main.js untouched,
// 'bundle' builds main.tsx through Vite.
export const VIEWS = [
  { mod: 'osfui', name: 'handoff', mode: 'verbatim' },
  { mod: 'osfui', name: 'keybinds', mode: 'bundle' },
  { mod: 'osfui', name: 'settings', mode: 'bundle' },
];

// Every file this build owns. verify-output asserts the emitted set matches
// exactly, so a stray .map/fixture/orphaned chunk fails the build instead of
// shipping in the next archive.
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
