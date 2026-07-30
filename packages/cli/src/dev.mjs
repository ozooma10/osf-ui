import { realpath } from 'node:fs/promises';

import { mergeConfig } from 'vite';

import { harnessPlugin } from './harness-plugin.mjs';

/**
 * The `osfui dev` Vite config: harness plugin, merged with the
 * project's optional dev-only `vite:` extension (aliases, extra plugins).
 * mergeConfig appends project plugins after the harness plugin, so the
 * harness keeps ownership of /shared/* and /__osfui/*. Build and check never
 * read `vite:` — nothing here can change shipped output.
 */
export async function devServerConfig(project, view, options = {}) {
  const extra = typeof project.vite === 'function'
    ? await project.vite({ command: 'serve' })
    : (project.vite ?? {});
  // Vite can canonicalize Windows temp/project paths through their 8.3
  // aliases and then reject its own root — but `fs.strict: false` is not the
  // answer: it disables isFileServingAllowed AND isFileLoadingAllowed,
  // dropping both the allow root and the .env/*.pem/.git deny list, so any
  // file on the machine becomes readable via /@fs/. Allow the project root
  // under both spellings instead (covering `vite:` aliases like ./dep); the
  // default deny list stays active inside it.
  const allow = [project.root];
  try {
    const canonical = await realpath(project.root);
    if (canonical !== project.root) allow.push(canonical);
  } catch {
    // loadProject just resolved this root; keep the raw spelling.
  }
  return mergeConfig({
    root: project.viewsRoot,
    base: '/',
    plugins: [harnessPlugin(project, view)],
    server: {
      host: options.host || '127.0.0.1',
      port: Number(options.port) || 5173,
      open: options.open === 'false' ? false : '/__osfui/',
      fs: { allow },
    },
  }, extra);
}
