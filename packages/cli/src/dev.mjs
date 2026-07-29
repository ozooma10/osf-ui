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
  return mergeConfig({
    root: project.viewsRoot,
    base: '/',
    plugins: [harnessPlugin(project, view)],
    server: {
      host: options.host || '127.0.0.1',
      port: Number(options.port) || 5173,
      open: options.open === 'false' ? false : '/__osfui/',
      // Vite can canonicalize Windows temp/project paths through their 8.3
      // aliases and then reject its own root. The author server binds to
      // loopback by default, so disable that redundant filesystem check.
      fs: { strict: false },
    },
  }, extra);
}
