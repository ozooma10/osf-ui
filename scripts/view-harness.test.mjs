import assert from 'node:assert/strict';
import { promises as fs } from 'node:fs';
import { tmpdir } from 'node:os';
import { join } from 'node:path';
import test from 'node:test';

import {
  createViewHarness,
  injectBootstrap,
  loadViewDefinition,
  safeResolve,
  VIEW_CSP,
} from './view-harness-lib.mjs';

async function fixtureView(t, manifest = {}) {
  const root = await fs.mkdtemp(join(tmpdir(), 'osfui-view-harness-'));
  t.after(() => fs.rm(root, { force: true, recursive: true }));
  const view = join(root, 'views', 'acme.widgets', 'panel');
  await fs.mkdir(view, { recursive: true });
  await fs.writeFile(join(view, 'manifest.json'), JSON.stringify({
    id: 'panel',
    title: 'Widget Panel',
    entry: 'index.html',
    width: 1280,
    height: 720,
    permissions: { nativeBridge: true },
    ...manifest,
  }));
  await fs.writeFile(join(view, 'index.html'),
    '<!doctype html><html><head><script src="first.js"></script></head><body>Panel</body></html>');
  await fs.writeFile(join(view, 'first.js'), 'window.firstLoaded = true;');
  return view;
}

test('loadViewDefinition derives the qualified id and production dimensions', async (t) => {
  const view = await fixtureView(t);
  const definition = await loadViewDefinition(view);
  assert.equal(definition.qualifiedId, 'acme.widgets/panel');
  assert.equal(definition.width, 1280);
  assert.equal(definition.height, 720);
  assert.equal(definition.nativeBridge, true);
});

test('loadViewDefinition rejects a copied manifest with the wrong id', async (t) => {
  const view = await fixtureView(t, { id: 'other' });
  await assert.rejects(loadViewDefinition(view), /must equal view folder "panel"/);
});

test('injectBootstrap runs the harness before authored head scripts', () => {
  const html = '<html><head><script src="main.js"></script></head></html>';
  const injected = injectBootstrap(html);
  assert.ok(injected.indexOf('/__osfui/bootstrap.js') < injected.indexOf('main.js'));
});

test('safeResolve keeps requests inside the mapped views root', () => {
  const root = process.platform === 'win32' ? 'C:\\views' : '/views';
  assert.ok(safeResolve(root, '/acme.widgets/panel/index.html'));
  assert.equal(safeResolve(root, '/../secret.txt'), null);
  assert.equal(safeResolve(root, '/%2e%2e/secret.txt'), null);
});

test('standalone server injects the bridge bootstrap, CSP, shared kit, and fixtures', async (t) => {
  const view = await fixtureView(t);
  await fs.writeFile(join(view, 'osfui.mock.json'), JSON.stringify({
    state: { player: { name: 'Morgan' } },
    requests: { 'acme.widgets.get': { ok: true } },
  }));
  const harness = await createViewHarness(view, { port: 0 });
  t.after(() => harness.close());

  const meta = await fetch(new URL('meta.json', harness.url)).then((response) => response.json());
  assert.equal(meta.qualifiedId, 'acme.widgets/panel');
  assert.equal(meta.viewUrl, '/acme.widgets/panel/index.html');

  const entryResponse = await fetch(new URL(meta.viewUrl, harness.url));
  const entry = await entryResponse.text();
  assert.equal(entryResponse.headers.get('content-security-policy'), VIEW_CSP);
  assert.ok(entry.includes('/__osfui/bootstrap.js'));
  assert.ok(entry.indexOf('/__osfui/bootstrap.js') < entry.indexOf('first.js'));

  const kit = await fetch(new URL('/shared/osfui.js', harness.url));
  assert.equal(kit.status, 200);
  assert.match(await kit.text(), /g\.ready = new Promise/);

  const fixture = await fetch(new URL('/__osfui/fixture.json', harness.url))
    .then((response) => response.json());
  assert.equal(fixture.state.player.name, 'Morgan');
});

test('malformed mock fixture is returned as a visible harness error', async (t) => {
  const view = await fixtureView(t);
  await fs.writeFile(join(view, 'osfui.mock.json'), '{ nope');
  const harness = await createViewHarness(view, { port: 0 });
  t.after(() => harness.close());
  const fixture = await fetch(new URL('fixture.json', harness.url))
    .then((response) => response.json());
  assert.match(fixture.$error, /Cannot parse osfui\.mock\.json/);
});
