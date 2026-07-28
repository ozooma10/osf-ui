// osfui.config.ts mirrors the per-view manifest.json files for the `osfui
// dev` server; the manifests stay the shipped-artifact source of truth. This
// pins the two against each other so they cannot drift.

import { readFileSync, readdirSync, existsSync } from 'node:fs';
import { resolve } from 'node:path';
import { describe, expect, it } from 'vitest';

import config from '../osfui.config';

const VIEWS_DIR = resolve(__dirname, '../src/views/osfui');

function manifests(): Record<string, { title: string; description: string }> {
  const out: Record<string, { title: string; description: string }> = {};
  for (const dir of readdirSync(VIEWS_DIR)) {
    const path = resolve(VIEWS_DIR, dir, 'manifest.json');
    if (existsSync(path)) out[dir] = JSON.parse(readFileSync(path, 'utf8'));
  }
  return out;
}

describe('osfui.config.ts vs manifest.json parity', () => {
  const byId = new Map((config.views ?? []).map((view) => [view.id, view]));
  const shipped = manifests();

  it('declares every built-in view exactly once, and nothing else', () => {
    expect([...byId.keys()].sort()).toEqual(Object.keys(shipped).sort());
    expect(byId.size).toBe((config.views ?? []).length);
  });

  it('is the osfui project with the settings surface first', () => {
    expect(config.modId).toBe('osfui');
    expect(config.views?.[0]?.id).toBe('settings');
  });

  for (const [id, manifest] of Object.entries(manifests())) {
    it(`${id}: title and description match the manifest`, () => {
      const view = byId.get(id);
      expect(view?.title).toBe(manifest.title);
      expect(view?.description).toBe(manifest.description);
    });
  }
});
