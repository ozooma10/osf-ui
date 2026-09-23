
import { existsSync, readdirSync, statSync } from 'node:fs';
import { dirname, join, relative, resolve } from 'node:path';
import { fileURLToPath } from 'node:url';

const HERE = dirname(fileURLToPath(import.meta.url));
export const FRONTEND = resolve(HERE, '..');
export const REPO = resolve(FRONTEND, '..');

export const OUT = process.env.OSFUI_VIEWS_OUT
  ? resolve(process.env.OSFUI_VIEWS_OUT)
  : join(REPO, 'build', 'frontend', 'views');

export function expectedOutputs() {
  return ['shared/gamepadnav.js', 'shared/osfui.css', 'shared/osfui.js'];
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
