// The shipped-file-set gate. scripts/verify-output.mjs also runs inside
// `npm run build` (build.mjs and this file are its only callers); re-running it
// here names the failure in CI and re-checks output hand-edited after the build.
//
// The assertions below duplicate some of verifyOutput()'s checks on purpose: if
// it regresses to returning [] early (swallowed throw, OUT path resolving
// nowhere) the "no problems" assertion passes vacuously. These name the files.

import { describe, it, expect } from 'vitest';
import { readFileSync } from 'node:fs';
import { join } from 'node:path';
import { OUT, FRONTEND, VIEWS, expectedOutputs, walk } from '../scripts/config.mjs';
import { verifyOutput } from '../scripts/verify-output.mjs';

describe('build output', () => {
  it('passes every gate in verify-output.mjs', () => {
    const problems = verifyOutput();
    // Assert on the array itself so a failure prints the problem text.
    expect(problems).toEqual([]);
  });

  it('emits exactly the expected file set', () => {
    // Equality in both directions. A missing file breaks the view; an unexpected
    // one ships forever — neither tools/package.ps1's recursive staging copy nor
    // the after_build MO2 redeploy (os.cp overlays, never prunes) can remove a
    // file it once wrote.
    expect(walk(OUT).sort()).toEqual(expectedOutputs());
  });

  it('emits no source maps', () => {
    // Covered by the set-equality above, but stated separately: nothing in
    // package.ps1 or CI excludes by extension, so a .map ships in the public
    // archive along with the TSX paths embedded in it.
    expect(walk(OUT).filter((f: string) => f.endsWith('.map'))).toEqual([]);
  });

  // Frozen public contract: shared/osfui.{js,css} are bridge protocol 1.0
  // (api-freeze item 5) and third-party mods link `../../shared/osfui.js` by that
  // exact path; padnav.js is private-but-unfrozen, shipped as-is pending in-game
  // controller verification (frontend/COMPATIBILITY.md). All three are copied,
  // never regenerated, so byte-identical is the whole spec.
  const verbatim: Array<[string, string]> = [
    ['src/shared-kit/osfui.js', 'shared/osfui.js'],
    ['src/shared-kit/osfui.css', 'shared/osfui.css'],
    ['src/legacy/padnav.js', 'osfui/padnav.js'],
  ];

  it.each(verbatim)('%s is copied byte-identically to %s', (src, out) => {
    const a = readFileSync(join(FRONTEND, src));
    const b = readFileSync(join(OUT, out));
    // Buffers, not strings: a UTF-8 BOM or a CRLF/LF rewrite must fail even
    // though both sides decode to the same text.
    expect(b.equals(a)).toBe(true);
  });

  describe.each(VIEWS)('$mod/$name/index.html', (v) => {
    const html = () => readFileSync(join(OUT, v.mod, v.name, 'index.html'), 'utf8');

    it('does not use type="module"', () => {
      // shared/osfui.js owns osfui.onMessage and must execute before main.js;
      // modules are deferred, which silently inverts that order even if CORS
      // passed.
      expect(html()).not.toMatch(/type\s*=\s*["']module["']/);
    });

    it('has no crossorigin attribute', () => {
      // Vite's HTML pipeline injects `crossorigin` alongside `type="module"`, so
      // its presence means index.html was processed rather than copied — which
      // also rewrites hrefs against `base` and hashes assets, breaking the
      // `../../shared/osfui.css` relative depth docs/authoring-views.md promises
      // third-party view authors.
      expect(html()).not.toMatch(/\bcrossorigin\b/);
    });
  });
});
