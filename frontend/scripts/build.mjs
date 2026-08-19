
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

  rmSync(OUT, { recursive: true, force: true });

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
            entryFileNames: 'main.js',
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
