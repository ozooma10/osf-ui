// Boots the real `osfui dev` server against this project (osfui.config.ts +
// osfui.mock.ts + builtin-dev-plugin) and asserts the legacy-contract shims:
// classic main.js tags become module main.tsx in dev, padnav is served at the
// path the pages reference, the mock module transforms through Vite with its
// aliases, and the harness lists all four views.

import { resolve } from 'node:path';
import { createServer, type ViteDevServer } from 'vite';
import { afterAll, beforeAll, describe, expect, it } from 'vitest';

// Embedding entry points of the CLI package — the osfui bin drives dev
// through the same two functions.
import { loadProject } from '@osfui/cli/config';
import { devServerConfig } from '@osfui/cli/dev';

const FRONTEND = resolve(__dirname, '..');

describe('osfui dev serves the built-in views', () => {
  let server: ViteDevServer;
  let origin: string;

  beforeAll(async () => {
    const project = await loadProject(FRONTEND);
    const config = await devServerConfig(project, project.views[0], { open: 'false', port: 0 });
    server = await createServer({ ...config, logLevel: 'silent' });
    await server.listen();
    const address = server.httpServer!.address() as { port: number };
    origin = `http://127.0.0.1:${address.port}`;
  }, 60000);

  afterAll(async () => {
    await server?.close();
  });

  it('rewrites the classic entry to the module source, bootstrap + loader first', async () => {
    const html = await fetch(`${origin}/osfui/settings/index.html`).then((r) => r.text());
    expect(html).toContain('<script type="module" src="./main.tsx"></script>');
    expect(html).not.toContain('<script src="main.js"></script>');
    const bootstrapAt = html.indexOf('/__osfui/bootstrap.js');
    const loaderAt = html.indexOf('/__osfui/mock-loader.js');
    const kitAt = html.indexOf('../../shared/osfui.js');
    const entryAt = html.indexOf('./main.tsx');
    expect(bootstrapAt).toBeGreaterThan(-1);
    expect(loaderAt).toBeGreaterThan(bootstrapAt);
    expect(kitAt).toBeGreaterThan(loaderAt);
    expect(entryAt).toBeGreaterThan(kitAt);
    // The page's inline meta names this view and advertises the mock.
    expect(html).toContain('"qualifiedId":"osfui/settings"');
    expect(html).toContain('"mockUrl":"/__osfui/mock-entry.js"');
  });

  it('serves padnav at the path the pages reference', async () => {
    const response = await fetch(`${origin}/osfui/padnav.js`);
    expect(response.status).toBe(200);
    expect(await response.text()).toContain('padnav');
  });

  it('serves the shared kit for the exact ../../shared/ URLs', async () => {
    const kit = await fetch(`${origin}/shared/osfui.js`).then((r) => r.text());
    expect(kit).toContain('osfui');
    const css = await fetch(`${origin}/shared/osfui.css`);
    expect(css.status).toBe(200);
  });

  it('transforms osfui.mock.ts (and its @devmock graph) through Vite', async () => {
    const entry = await fetch(`${origin}/__osfui/mock-entry.js`);
    expect(entry.status).toBe(200);
    const source = await entry.text();
    expect(source).toContain('install');
    expect(source).not.toContain(': MockContext');
    // The mockbridge import resolved to a served module URL, not a bare path.
    expect(source).toMatch(/from\s+"[^"]*mockbridge/);
  });

  it('lists all four built-in views for the shell switcher', async () => {
    const listing = await fetch(`${origin}/__osfui/meta.json`).then((r) => r.json());
    expect(listing.initial).toBe('osfui/settings');
    expect(listing.views.map((view: { qualifiedId: string }) => view.qualifiedId).sort()).toEqual([
      'osfui/benchmark',
      'osfui/handoff',
      'osfui/keybinds',
      'osfui/settings',
    ]);
    for (const view of listing.views) {
      expect(view.width).toBe(1600);
      expect(view.height).toBe(900);
    }
  });

  it('transforms each view entry module without errors', async () => {
    for (const view of ['settings', 'keybinds', 'benchmark', 'handoff']) {
      const response = await fetch(`${origin}/osfui/${view}/main.tsx`);
      expect(response.status, view).toBe(200);
      expect(await response.text(), view).toContain('render');
    }
  }, 30000);

  it('serves the OSF Animation preview page at /osf.html', async () => {
    const page = await fetch(`${origin}/osf.html`);
    expect(page.status).toBe(200);
    expect(await page.text()).toContain('OSF ANIMATION');
  });
});
