// OSF UI frontend build orchestrator.
//
// Output defaults to the ignored `build/frontend/views/**` tree. Source is the
// only frontend content tracked in git; xmake deployment/install and the release
// packager consume this generated tree.
//
// One build per view, not one `vite build`: Rollup refuses IIFE with multiple
// inputs ("UMD and IIFE output formats are not supported for code-splitting
// builds") and `inlineDynamicImports` allows only a single input.
//
// index.html is copied, never run through Vite: the HTML pipeline rewrites
// script/link hrefs against `base`, injects `type="module"` + `crossorigin`, and
// hashes assets. Views must keep the exact tag shape and relative depth
// `../../shared/osfui.css` that docs/authoring-views.md promises to third-party
// authors.

import { build } from 'vite';
import { copyFileSync, mkdirSync, rmSync, writeFileSync } from 'node:fs';
import { dirname, join } from 'node:path';
import { BUILD_VIEWS, FRONTEND, OUT, expectedOutputs } from './config.mjs';
import { composeHelper } from './compose-helper.mjs';

function copy(from, to) {
  mkdirSync(dirname(to), { recursive: true });
  copyFileSync(from, to);
}

export async function runBuild({ quiet = false } = {}) {
  const log = quiet ? () => {} : (m) => console.log(m);

  // 1. This directory is build-owned and ignored, so remove it wholesale. That
  //    guarantees renamed/removed outputs cannot leak into installs.
  rmSync(OUT, { recursive: true, force: true });

  // 2. Public/shared assets.
  //    - shared/osfui.js is the untouched authored 2.0 helper followed by the
  //      guarded temporary v1 facade. The composition hook is removed with
  //      frontend/src/compat/v1 in 2.1.0.
  //    - padnav.js is private but unfrozen. It reads concrete DOM geometry and
  //      its in-game controller verification is still pending, so it ships
  //      as-is. Exit criteria in frontend/COMPATIBILITY.md.
  mkdirSync(join(OUT, 'shared'), { recursive: true });
  writeFileSync(join(OUT, 'shared/osfui.js'), composeHelper(), 'utf8');
  copy(join(FRONTEND, 'src/shared-kit/osfui.css'), join(OUT, 'shared/osfui.css'));
  copy(join(FRONTEND, 'src/legacy/padnav.js'), join(OUT, 'osfui/padnav.js'));
  log('  composed shared helper; copied stylesheet + padnav');

  for (const v of BUILD_VIEWS) {
    const src = join(FRONTEND, 'src/views', v.mod, v.name);
    const dst = join(OUT, v.mod, v.name);
    copy(join(src, 'index.html'), join(dst, 'index.html'));
    copy(join(src, 'manifest.json'), join(dst, 'manifest.json'));
  }

  // 3. One single-entry IIFE build per view.
  for (const v of BUILD_VIEWS) {
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
            // No inlineDynamicImports: rolldown-vite already disables code
            // splitting for single-input IIFE builds, and warns if it is set.
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

// Run directly (`node scripts/build.mjs`), not when imported by another tool.
if (process.argv[1] && process.argv[1].endsWith('build.mjs')) {
  console.log('OSF UI frontend -> build/frontend/views');
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
