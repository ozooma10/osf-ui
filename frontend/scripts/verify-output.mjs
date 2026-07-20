// Post-build gates on data/OSFUI/views. Run from `npm run build` so a bad build
// never reaches the tree, and again from frontend/test/build.*.test.ts so CI
// reports them as tests. Each gate catches a constraint that would otherwise
// only fail in game, as a blank overlay plus a console error nobody reads.

import { readFileSync, existsSync } from 'node:fs';
import { join } from 'node:path';
import { OUT, FRONTEND, VIEWS, expectedOutputs, walk } from './config.mjs';

// Identifiers that exist only in the dev harness. Any of them in a shipped
// bundle means the DEV-branch dead-code elimination silently stopped working.
const DEV_SENTINELS = [
  'OSFUI_MOCK_BRIDGE',
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
  // Nothing in package.ps1 or CI excludes by extension, so a stray .map would
  // ship in every archive.
  for (const f of actual) if (f.endsWith('.map')) fail(`source map in shipped output: ${f}`);

  // Verbatim artifacts must stay byte-identical to their sources.
  const verbatim = [
    ['src/shared-kit/osfui.js', 'shared/osfui.js'],
    ['src/shared-kit/osfui.css', 'shared/osfui.css'],
    ['src/legacy/padnav.js', 'osfui/padnav.js'],
  ];
  for (const [src, out] of verbatim) {
    const a = join(FRONTEND, src), b = join(OUT, out);
    if (!existsSync(a) || !existsSync(b)) { fail(`verbatim pair missing: ${src} -> ${out}`); continue; }
    if (!readFileSync(a).equals(readFileSync(b))) fail(`verbatim artifact drifted: ${out} != ${src}`);
  }

  for (const v of VIEWS) {
    const dir = join(OUT, v.mod, v.name);
    const html = join(dir, 'index.html');
    const css = join(dir, 'style.css');
    const js = join(dir, 'main.js');

    // Classic scripts only: the built-in views stay on one stable classic IIFE
    // bundle to hold the load-order contract — shared/osfui.js must execute
    // before main.js and owns osfui.onMessage. Modules are deferred.
    if (existsSync(html)) {
      const h = readFileSync(html, 'utf8');
      if (/type\s*=\s*["']module["']/.test(h)) fail(`${v.name}/index.html uses type="module" (built-in bundles must remain classic IIFEs)`);
      if (/\bcrossorigin\b/.test(h)) fail(`${v.name}/index.html has a crossorigin attribute (Vite HTML pipeline leaked in)`);
      if (!/src="\.\.\/\.\.\/shared\/osfui\.js"/.test(h)) fail(`${v.name}/index.html no longer loads ../../shared/osfui.js`);
      if (!/href="\.\.\/\.\.\/shared\/osfui\.css"/.test(h)) fail(`${v.name}/index.html no longer links ../../shared/osfui.css`);
      const kit = h.indexOf('shared/osfui.js'), main = h.indexOf('src="main.js"');
      if (kit >= 0 && main >= 0 && kit > main) fail(`${v.name}/index.html loads main.js before the shared kit`);
    }

    // Network-free and self-contained: permissions.network is force-disabled
    // natively (ViewManifest.cpp) and all three --osf-font-* stacks resolve to
    // Windows system faces.
    if (existsSync(css)) {
      const c = readFileSync(css, 'utf8');
      if (/@font-face/.test(c)) fail(`${v.name}/style.css contains @font-face (views must ship zero webfont binaries)`);
      if (/url\(\s*["']?https?:/i.test(c)) fail(`${v.name}/style.css loads a remote URL`);
      // The D3D12 compositor expects premultiplied BGRA; the page's transparent
      // body is what supplies it. An opaque html/body background renders the
      // overlay as a black rectangle over the game.
      // The Oxc minifier rewrites `background: transparent` to the shorthand
      // `background: 0 0` (position 0 0, everything else initial — initial
      // background-color is transparent), so bare `0` must pass. The target is a
      // colour: #hex, rgb()/hsl(), or a named colour.
      if (/\b(html|body)\s*\{[^}]*background(-color)?\s*:\s*(?!none\b|transparent\b|inherit\b|0[\s;}])(#|rgba?\(|hsla?\(|[a-z])/i.test(c)) {
        fail(`${v.name}/style.css sets an opaque background on html/body (would black out the overlay)`);
      }
    }

    // No dev-only code survived into the bundle. Scoped to `mode: 'bundle'`
    // views: a verbatim copy has no DEV elimination to verify, and hand-written
    // views do ship harness code (sampleMods()/sampleViews() fixtures, a
    // window.OSFUI_MOD_ASSET_ROOTS read in the asset-path sanitiser), so the
    // gate would only block them.
    if (existsSync(js)) {
      const j = readFileSync(js, 'utf8');
      if (v.mode === 'bundle') {
        for (const s of DEV_SENTINELS) {
          if (j.includes(s)) fail(`${v.name}/main.js contains dev-only identifier "${s}" (import.meta.env.DEV elimination failed)`);
        }
      }
      if (/url\(\s*["']?https?:|["']https?:\/\/(?!osfui\.local)/.test(j)) {
        fail(`${v.name}/main.js references a remote URL`);
      }
    }
  }

  return problems;
}
