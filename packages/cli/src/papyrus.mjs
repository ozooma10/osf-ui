import { readdir, stat } from 'node:fs/promises';
import { relative, resolve, sep } from 'node:path';

async function pscFiles(root) {
  let entries;
  try {
    entries = await readdir(root, { withFileTypes: true });
  } catch {
    return [];
  }
  const result = [];
  for (const entry of entries) {
    const path = resolve(root, entry.name);
    if (entry.isDirectory()) result.push(...await pscFiles(path));
    else if (entry.name.toLowerCase().endsWith('.psc')) result.push(path);
  }
  return result;
}

/**
 * Missing/stale compiled Papyrus scripts under the project's Data root.
 *
 * `<modRoot>/Scripts/Source[/User]/<Namespace…>/<Name>.psc` compiles to
 * `<modRoot>/Scripts/<Namespace…>/<Name>.pex`, and the game only loads the
 * `.pex`. The compiler ships with the Creation Kit, so the CLI cannot run it —
 * these are advisory warnings, never build failures: a forgotten compile shows
 * up here instead of as a silently dead backend in game, but mtimes are only a
 * heuristic (a fresh checkout can write the source after its compiled file).
 */
export async function papyrusWarnings(modRoot) {
  const sourceRoot = resolve(modRoot, 'Scripts', 'Source');
  const warnings = [];
  for (const psc of await pscFiles(sourceRoot)) {
    let rel = relative(sourceRoot, psc);
    if (rel.toLowerCase().startsWith(`user${sep}`)) rel = rel.slice(`user${sep}`.length);
    const pex = resolve(modRoot, 'Scripts', rel.replace(/\.psc$/i, '.pex'));
    const sourceName = relative(modRoot, psc).replaceAll(sep, '/');
    const compiledName = relative(modRoot, pex).replaceAll(sep, '/');
    try {
      const pscStat = await stat(psc);
      try {
        if ((await stat(pex)).mtimeMs < pscStat.mtimeMs) {
          warnings.push(`${compiledName} is older than ${sourceName} - recompile it, or the game keeps running the previous script.`);
        }
      } catch {
        warnings.push(`${compiledName} is missing - compile ${sourceName}, or the Papyrus backend will not run in game.`);
      }
    } catch {}
  }
  return warnings;
}

export async function reportPapyrus(project) {
  for (const warning of await papyrusWarnings(project.modRoot)) {
    console.warn(`[osfui] WARN: ${warning}`);
  }
}
