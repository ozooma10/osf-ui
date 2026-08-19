import { realpath } from 'node:fs/promises';

import { harnessPlugin } from './harness-plugin.mjs';

export async function devServerConfig(project, view, options = {}) {
  const allow = [project.root];
  try {
    const canonical = await realpath(project.root);
    if (canonical !== project.root) allow.push(canonical);
  } catch {}
  return {
    root: project.viewsRoot,
    base: '/',
    plugins: [harnessPlugin(project, view)],
    server: {
      host: options.host || '127.0.0.1',
      port: Number(options.port) || 5173,
      open: options.open === 'false' ? false : '/__osfui/',
      fs: { allow },
    },
  };
}
