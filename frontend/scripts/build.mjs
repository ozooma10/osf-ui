// build.mjs — the OSF UI frontend build orchestrator.
//
// Emits `data/OSFUI/views/**` EXACTLY: the shipped view directories are this
// script's output, and three separate pipelines already treat that path as
// authoritative (xmake `add_installfiles("data/(OSFUI/**)")`, the after_build
// MO2 redeploy, and tools/package.ps1's recursive staging copy). Emitting
// anywhere else would mean editing all three; emitting here means editing none.
//
// Why an orchestrator instead of one `vite build`:
//   Rollup refuses IIFE with multiple inputs ("UMD and IIFE output formats are
//   not supported for code-splitting builds"), and `inlineDynamicImports` only
//   permits a single input. Two views => two separate single-entry builds.
//
// Why index.html is copied, never processed by Vite:
//   Vite's HTML pipeline rewrites script/link hrefs against `base`, injects
//   `type="module"` + `crossorigin`, and hashes assets. The views must keep the
//   exact tag shape and the exact relative depth `../../shared/osfui.css` that
//   docs/authoring-views.md promises to third-party authors.

import { build } from 'vite';
import { copyFileSync, mkdirSync, rmSync } from 'node:fs';
import { dirname, join } from 'node:path';
import { FRONTEND, OUT, VIEWS, expectedOutputs } from './config.mjs';

function copy(from, to) {
  mkdirSync(dirname(to), { recursive: true });
  copyFileSync(from, to);
}

export async function runBuild({ quiet = false } = {}) {
  const log = quiet ? () => {} : (m) => console.log(m);

  // 1. Clean ONLY the paths we own. Never wipe the whole views root: a
  //    third-party mod installed into data/ for local testing must survive.
  for (const rel of expectedOutputs()) rmSync(join(OUT, rel), { force: true });

  // 2. Verbatim copies.
  //    - shared/osfui.{js,css} are the FROZEN public contract (bridge protocol
  //      1.0, api-freeze item 5). Third-party mods link `../../shared/osfui.js`
  //      by exact path; regenerating risks byte-level behaviour change against
  //      unknown consumers for zero user-visible gain.
  //    - padnav.js is private-but-unfrozen: its own header calls it
  //      "deliberately PRIVATE to the osfui views". It reads concrete DOM
  //      geometry and its in-game controller verification is still pending, so
  //      it ships as-is. See frontend/COMPATIBILITY.md for the exit criteria.
  copy(join(FRONTEND, 'src/shared-kit/osfui.js'), join(OUT, 'shared/osfui.js'));
  copy(join(FRONTEND, 'src/shared-kit/osfui.css'), join(OUT, 'shared/osfui.css'));
  copy(join(FRONTEND, 'src/legacy/padnav.js'), join(OUT, 'osfui/padnav.js'));
  log('  copied shared kit + padnav');

  for (const v of VIEWS) {
    const src = join(FRONTEND, 'src/views', v.mod, v.name);
    const dst = join(OUT, v.mod, v.name);
    copy(join(src, 'index.html'), join(dst, 'index.html'));
    copy(join(src, 'manifest.json'), join(dst, 'manifest.json'));
    if (v.mode === 'verbatim') {
      copy(join(src, 'main.legacy.js'), join(dst, 'main.js'));
      copy(join(src, 'style.css'), join(dst, 'style.css'));
      log(`  ${v.mod}/${v.name}: verbatim`);
    }
  }

  // 3. One single-entry IIFE build per bundled view.
  for (const v of VIEWS.filter((x) => x.mode === 'bundle')) {
    await build({
      configFile: join(FRONTEND, 'vite.config.ts'),
      root: FRONTEND,
      logLevel: quiet ? 'silent' : 'warn',
      build: {
        outDir: join(OUT, v.mod, v.name),
        emptyOutDir: false, // step 1 already cleaned exactly what we own
        rollupOptions: {
          input: join(FRONTEND, 'src/views', v.mod, v.name, 'main.tsx'),
          output: {
            format: 'iife',
            // No inlineDynamicImports: rolldown-vite disables code splitting
            // for single-input IIFE builds already (and warns if it is set).
            // The build.syntax gate (sourceType: 'script') still proves no
            // import/export survives into the bundle.
            entryFileNames: 'main.js',
            // Stable names only. Content hashes would orphan files in the MO2
            // mod folder (the after_build redeploy uses os.cp, which overlays
            // and never prunes) and would break the byte-diff stale check.
            assetFileNames: (a) =>
              (a.names?.[0] ?? a.name ?? '').endsWith('.css') ? 'style.css' : '[name][extname]',
          },
        },
      },
    });
    log(`  ${v.mod}/${v.name}: bundled`);
  }
}

// Run directly (`node scripts/build.mjs`), not when imported by check-dist.
if (process.argv[1] && process.argv[1].endsWith('build.mjs')) {
  console.log('OSF UI frontend -> data/OSFUI/views');
  await runBuild();
  const { verifyOutput } = await import('./verify-output.mjs');
  const problems = verifyOutput();
  if (problems.length) {
    console.error('\nBuild verification FAILED:');
    for (const p of problems) console.error('  - ' + p);
    process.exit(1);
  }
  console.log(`OK  ${expectedOutputs().length} files verified`);
}
