import { readFile, realpath } from 'node:fs/promises';
import { mergeConfig } from 'vite';

import { harnessPlugin } from './harness-plugin.mjs';

const HARNESS_MODULE_PREFIX = '/__osfui/';
const HARNESS_MODULE_ID = '\0osfui-authoring-harness:';
const HARNESS_MODULES = new Set(['mock-loader.js', 'mock-runtime.js', 'tools-model.js']);

function harnessModuleResolver() {
  return {
    name: 'osfui-authoring-harness-modules',
    enforce: 'pre',
    resolveId(source, importer) {
      if (importer?.startsWith(HARNESS_MODULE_ID) && source.startsWith('./')) {
        const name = source.slice(2);
        return HARNESS_MODULES.has(name) ? `${HARNESS_MODULE_ID}${name}` : null;
      }
      const path = source.split('?')[0];
      if (!path.startsWith(HARNESS_MODULE_PREFIX)) return null;
      const name = path.slice(HARNESS_MODULE_PREFIX.length);
      if (!HARNESS_MODULES.has(name)) return null;
      return `${HARNESS_MODULE_ID}${name}`;
    },
    load(id) {
      if (!id.startsWith(HARNESS_MODULE_ID)) return null;
      return readFile(new URL(`./browser/${id.slice(HARNESS_MODULE_ID.length)}`, import.meta.url), 'utf8');
    },
  };
}

export async function devServerConfig(project, view, options = {}) {
  const allow = [project.root];
  let servedRoot = project.viewsRoot;
  try {
    const canonical = await realpath(project.root);
    if (canonical !== project.root) allow.push(canonical);
  } catch {}
  try {
    servedRoot = await realpath(project.viewsRoot);
  } catch {}
  const configured = mergeConfig(project.vite, {
    plugins: [harnessModuleResolver(), harnessPlugin(project, view)],
  });
  return {
    ...configured,
    // The harness owns the served tree and navigation entry. Project Vite
    // options may extend compilation, but cannot escape the view workspace.
    root: servedRoot,
    base: '/',
    server: {
      ...configured.server,
      host: options.host || '127.0.0.1',
      port: Number(options.port) || 5173,
      open: options.open === 'false' ? false : '/__osfui/',
      fs: { ...configured.server?.fs, allow },
    },
  };
}
