
import { existsSync, readFileSync, readdirSync, statSync } from 'node:fs';
import { dirname, join, relative, resolve } from 'node:path';
import { fileURLToPath } from 'node:url';

const HERE = dirname(fileURLToPath(import.meta.url));
export const FRONTEND = resolve(HERE, '..');
export const REPO = resolve(FRONTEND, '..');

export const OUT = process.env.OSFUI_VIEWS_OUT
  ? resolve(process.env.OSFUI_VIEWS_OUT)
  : join(REPO, 'build', 'frontend', 'views');

const VIEWS_ROOT = join(FRONTEND, 'src', 'views');
export const VIEWS = readdirSync(VIEWS_ROOT, { withFileTypes: true })
  .filter((entry) => entry.isDirectory())
  .flatMap((modEntry) => {
    const modRoot = join(VIEWS_ROOT, modEntry.name);
    return readdirSync(modRoot, { withFileTypes: true })
      .filter((entry) => entry.isDirectory())
      .map((viewEntry) => {
        const manifestPath = join(modRoot, viewEntry.name, 'manifest.json');
        const manifest = JSON.parse(readFileSync(manifestPath, 'utf8'));
        if (manifest.mod !== modEntry.name) {
          throw new Error(
            `${relative(FRONTEND, manifestPath)} mod must match its source directory`,
          );
        }
        return { mod: modEntry.name, name: viewEntry.name, manifest };
      });
  })
  .sort((a, b) => `${a.mod}/${a.name}`.localeCompare(`${b.mod}/${b.name}`));

export const BUILD_VIEWS = VIEWS.filter((view) => view.manifest.debugOnly !== true);

export function expectedOutputs() {
  const files = ['shared/osfui.js', 'shared/osfui.css', 'osfui/padnav.js'];
  for (const v of BUILD_VIEWS) {
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
