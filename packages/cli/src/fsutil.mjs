// Small filesystem helpers shared across the CLI. Each of these existed as
// two or three per-file copies (with three different levels of correctness
// for `within`) before being folded here.
import { access, readdir, stat } from 'node:fs/promises';
import { isAbsolute, relative, resolve, sep } from 'node:path';

/** True when `path` names an existing filesystem entry. Falsy paths are absent. */
export async function exists(path) {
  if (!path) return false;
  return access(path).then(() => true, () => false);
}

/** Newest mtime under `path` (recursive); 0 when it does not exist. */
export async function latestMtime(path) {
  let info;
  try {
    info = await stat(path);
  } catch {
    return 0;
  }
  if (!info.isDirectory()) return info.mtimeMs;
  let latest = info.mtimeMs;
  for (const entry of await readdir(path, { withFileTypes: true })) {
    latest = Math.max(latest, await latestMtime(resolve(path, entry.name)));
  }
  return latest;
}

/**
 * True when `path` is `root` itself or inside it. Checks whole path segments:
 * a sibling entry whose name merely starts with ".." (e.g. "..notes") is
 * outside, and `<root>/..notes` is inside — the naive startsWith('..') both
 * of the previous copies used gets those backwards.
 */
export function within(root, path) {
  const child = relative(root, path);
  if (child === '') return true;
  return !isAbsolute(child) && child !== '..' && !child.startsWith(`..${sep}`);
}

/**
 * `<modRoot>/Scripts/Source[/User]/<Namespace…>/<Name>.psc` compiles to
 * `<modRoot>/Scripts/<Namespace…>/<Name>.pex` — the game loads only the .pex.
 */
export function pexFor(modRoot, psc) {
  const sourceRoot = resolve(modRoot, 'Scripts/Source');
  let rel = relative(sourceRoot, psc);
  if (rel.toLowerCase().startsWith(`user${sep}`)) rel = rel.slice(`user${sep}`.length);
  return resolve(modRoot, 'Scripts', rel.replace(/\.psc$/i, '.pex'));
}
