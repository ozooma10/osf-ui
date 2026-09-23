
import { readFileSync, existsSync } from 'node:fs';
import { join } from 'node:path';
import { OUT, FRONTEND, expectedOutputs, walk } from './config.mjs';
import { composeHelper } from './compose-helper.mjs';

export function verifyOutput() {
  const problems = [];
  const fail = (m) => problems.push(m);

  // File set is exactly what build.mjs owns.
  const expected = expectedOutputs();
  const actual = walk(OUT).sort();
  for (const f of expected) if (!actual.includes(f)) fail(`missing output: ${f}`);
  for (const f of actual) {
    if (!expected.includes(f)) fail(`unexpected file in views output: ${f}`);
  }
  for (const f of actual) if (f.endsWith('.map')) fail(`source map in shipped output: ${f}`);

  const helper = join(OUT, 'shared/osfui.js');
  if (!existsSync(helper)) fail('composed helper missing: shared/osfui.js');
  else if (readFileSync(helper, 'utf8') !== composeHelper()) {
    fail('shipped helper drifted from the 2.0 core');
  }

  // Remaining verbatim artifacts must stay byte-identical to their sources.
  const verbatim = [
    ['src/shared-kit/osfui.css', 'shared/osfui.css'],
    ['src/shared-kit/gamepadnav.js', 'shared/gamepadnav.js'],
  ];
  for (const [src, out] of verbatim) {
    const a = join(FRONTEND, src), b = join(OUT, out);
    if (!existsSync(a) || !existsSync(b)) { fail(`verbatim pair missing: ${src} -> ${out}`); continue; }
    if (!readFileSync(a).equals(readFileSync(b))) fail(`verbatim artifact drifted: ${out} != ${src}`);
  }

  return problems;
}
