
import { readFileSync, existsSync } from 'node:fs';
import { join } from 'node:path';
import { BUILD_VIEWS, OUT, FRONTEND, expectedOutputs, walk } from './config.mjs';
import { composeHelper } from './compose-helper.mjs';

// Test-only identifiers that must never leak into a shipped bundle.
const DEV_SENTINELS = [
  'OSFUI_MOD_ASSET_ROOTS',
  'acme.shipworks',
  '1.0.0-mock',
];

export function verifyOutput() {
  const problems = [];
  const fail = (m) => problems.push(m);

  // File set is exactly what build.mjs owns.
  const expected = expectedOutputs();
  const actual = walk(OUT).sort();
  for (const f of expected) if (!actual.includes(f)) fail(`missing output: ${f}`);
  for (const f of actual) {
    if (!expected.includes(f)) fail(`unexpected file in views output: ${f} (add it to expectedOutputs() or stop emitting it)`);
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
    ['src/legacy/padnav.js', 'shared/gamepadnav.js'],
  ];
  for (const [src, out] of verbatim) {
    const a = join(FRONTEND, src), b = join(OUT, out);
    if (!existsSync(a) || !existsSync(b)) { fail(`verbatim pair missing: ${src} -> ${out}`); continue; }
    if (!readFileSync(a).equals(readFileSync(b))) fail(`verbatim artifact drifted: ${out} != ${src}`);
  }

  for (const v of BUILD_VIEWS) {
    const dir = join(OUT, v.mod, v.name);
    const html = join(dir, 'index.html');
    const css = join(dir, 'style.css');
    const js = join(dir, 'main.js');

    if (existsSync(html)) {
      const h = readFileSync(html, 'utf8');
      if (/type\s*=\s*["']module["']/.test(h)) fail(`${v.name}/index.html uses type="module" (built-in bundles must remain classic IIFEs)`);
      if (/\bcrossorigin\b/.test(h)) fail(`${v.name}/index.html has a crossorigin attribute (Vite HTML pipeline leaked in)`);
      if (!/src="\/shared\/osfui\.js"/.test(h)) fail(`${v.name}/index.html no longer loads /shared/osfui.js`);
      if (!/href="\/shared\/osfui\.css"/.test(h)) fail(`${v.name}/index.html no longer links /shared/osfui.css`);
      if (!/src="\/shared\/gamepadnav\.js"/.test(h)) fail(`${v.name}/index.html no longer loads /shared/gamepadnav.js`);
      const kit = h.indexOf('/shared/osfui.js'), main = h.indexOf('src="main.js"');
      if (kit >= 0 && main >= 0 && kit > main) fail(`${v.name}/index.html loads main.js before the shared kit`);
    }

    if (existsSync(css)) {
      const c = readFileSync(css, 'utf8');
      if (/@font-face/.test(c)) fail(`${v.name}/style.css contains @font-face (views must ship zero webfont binaries)`);
      if (/url\(\s*["']?https?:/i.test(c)) fail(`${v.name}/style.css loads a remote URL`);
      if (/\b(html|body)\s*\{[^}]*background(-color)?\s*:\s*(?!none\b|transparent\b|inherit\b|0[\s;}])(#|rgba?\(|hsla?\(|[a-z])/i.test(c)) {
        fail(`${v.name}/style.css sets an opaque background on html/body (would black out the overlay)`);
      }
    }

    if (existsSync(js)) {
      const j = readFileSync(js, 'utf8');
      for (const s of DEV_SENTINELS) {
        if (j.includes(s)) fail(`${v.name}/main.js contains dev-only identifier "${s}" (import.meta.env.DEV elimination failed)`);
      }
      if (/url\(\s*["']?https?:|["']https?:\/\/(?!osfui\.local)/.test(j)) {
        fail(`${v.name}/main.js references a remote URL`);
      }
    }
  }

  return problems;
}
