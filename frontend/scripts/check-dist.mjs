// Stale-generated-output gate for CI: rebuilds into a scratch directory and
// byte-compares against the committed data/OSFUI/views, exiting non-zero with a
// per-file report on any difference. Does not use `git diff`, so it also catches
// drift in a dirty tree and works before the output is committed.

import { mkdtempSync, rmSync, readFileSync } from 'node:fs';
import { tmpdir } from 'node:os';
import { join } from 'node:path';

const scratch = mkdtempSync(join(tmpdir(), 'osfui-checkdist-'));
// Read by config.mjs at import time - must be set before the dynamic imports.
process.env.OSFUI_VIEWS_OUT = scratch;

const { runBuild } = await import('./build.mjs');
const { walk, COMMITTED_OUT } = await import('./config.mjs');

try {
  await runBuild({ quiet: true });

  const fresh = walk(scratch).sort();
  const onDisk = walk(COMMITTED_OUT).sort();
  const problems = [];

  for (const f of fresh) {
    if (!onDisk.includes(f)) { problems.push(`MISSING from data/OSFUI/views: ${f}`); continue; }
    const a = readFileSync(join(scratch, f));
    const b = readFileSync(join(COMMITTED_OUT, f));
    if (!a.equals(b)) problems.push(`STALE: ${f} (${b.length} bytes on disk, ${a.length} bytes rebuilt)`);
  }
  for (const f of onDisk) {
    if (!fresh.includes(f)) problems.push(`ORPHANED in data/OSFUI/views: ${f} (build no longer emits it)`);
  }

  if (problems.length) {
    console.error('Generated view output is out of date:\n');
    for (const p of problems) console.error('  ' + p);
    console.error("\nRun 'npm --prefix frontend run build' and commit the result.");
    process.exit(1);
  }
  console.log(`check:dist OK — ${fresh.length} generated files match data/OSFUI/views`);
} finally {
  rmSync(scratch, { recursive: true, force: true });
}
