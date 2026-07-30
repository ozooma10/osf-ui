import { readdir, stat } from 'node:fs/promises';
import { relative, resolve, sep } from 'node:path';

import { latestMtime, pexFor } from './fsutil.mjs';

export async function pscFiles(root) {
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
 * `.pex`. These advisory warnings keep hand-authored projects useful; projects
 * with a `papyrus` config also compile automatically before native builds.
 * Mtimes remain only a heuristic (a fresh checkout can write the source after
 * its compiled file).
 */
export async function papyrusWarnings(modRoot, papyrus = null) {
  const sourceRoot = resolve(modRoot, 'Scripts', 'Source');
  const warnings = [];
  for (const psc of await pscFiles(sourceRoot)) {
    const pex = pexFor(modRoot, psc);
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
  if (papyrus) {
    try {
      const pluginStat = await stat(papyrus.outputPath);
      if (await latestMtime(papyrus.sourceDir) > pluginStat.mtimeMs) {
        warnings.push(`${papyrus.plugin} is older than its Spriggit source - run npm run build before testing in game.`);
      }
    } catch {
      warnings.push(`${papyrus.plugin} is missing - run npm run build to generate the playable plugin with Spriggit.`);
    }
  }
  return warnings;
}

export async function reportPapyrus(project) {
  for (const warning of await papyrusWarnings(project.modRoot, project.papyrus)) {
    console.warn(`[osfui] WARN: ${warning}`);
  }
}
