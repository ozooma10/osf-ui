
import { copyFileSync, mkdirSync, rmSync, writeFileSync } from 'node:fs';
import { join } from 'node:path';
import { FRONTEND, OUT, expectedOutputs } from './config.mjs';
import { composeHelper } from './compose-helper.mjs';

export function runBuild({ quiet = false } = {}) {
  const log = quiet ? () => {} : (m) => console.log(m);

  rmSync(OUT, { recursive: true, force: true });

  mkdirSync(join(OUT, 'shared'), { recursive: true });
  writeFileSync(join(OUT, 'shared/osfui.js'), composeHelper(), 'utf8');
  copyFileSync(join(FRONTEND, 'src/shared-kit/osfui.css'), join(OUT, 'shared/osfui.css'));
  copyFileSync(join(FRONTEND, 'src/shared-kit/gamepadnav.js'), join(OUT, 'shared/gamepadnav.js'));
  log('  composed shared helper; copied stylesheet + gamepad navigation');
}

// Run directly (`node scripts/build.mjs`), not when imported by another tool.
if (process.argv[1] && process.argv[1].endsWith('build.mjs')) {
  console.log('OSF UI frontend -> build/frontend/views');
  runBuild();
  const { verifyOutput } = await import('./verify-output.mjs');
  const problems = verifyOutput();
  if (problems.length) {
    console.error('\nBuild verification FAILED:');
    for (const p of problems) console.error('  - ' + p);
    process.exit(1);
  }
  console.log(`OK  ${expectedOutputs().length} files verified`);
}
