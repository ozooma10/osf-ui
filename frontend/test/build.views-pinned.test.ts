// scripts/config.mjs VIEWS is a hand-maintained enumeration of the built-in
// view set — the third such list (the source directories and the native
// config are the other two). Adding a view directory and forgetting VIEWS
// gave a fully-green `verify` and a view silently absent from the release;
// this pins the list to the directories that actually exist.

import { readdirSync, statSync } from 'node:fs';
import { resolve } from 'node:path';
import { describe, it, expect } from 'vitest';
import { VIEWS, FRONTEND } from '../scripts/config.mjs';

describe('built-in view enumeration', () => {
  it('VIEWS matches the view directories under src/views, exactly', () => {
    const viewsRoot = resolve(FRONTEND, 'src/views');
    const onDisk: string[] = [];
    for (const mod of readdirSync(viewsRoot)) {
      const modDir = resolve(viewsRoot, mod);
      if (!statSync(modDir).isDirectory()) continue;
      for (const name of readdirSync(modDir)) {
        if (statSync(resolve(modDir, name)).isDirectory()) onDisk.push(`${mod}/${name}`);
      }
    }
    const declared = (VIEWS as Array<{ mod: string; name: string }>)
      .map((view) => `${view.mod}/${view.name}`);
    expect(declared.sort()).toEqual(onDisk.sort());
  });
});
