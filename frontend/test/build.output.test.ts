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
import { BUILD_VIEWS, OUT, FRONTEND, expectedOutputs, walk } from '../scripts/config.mjs';
import { verifyOutput } from '../scripts/verify-output.mjs';
import { composeHelper } from '../scripts/compose-helper.mjs';

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

  it('deterministically composes the strict 2.0 helper and guarded v1 facade', () => {
    expect(readFileSync(join(OUT, 'shared/osfui.js'), 'utf8')).toBe(composeHelper());
  });

  // Published public contract: shared/osfui.css is copied verbatim,
  // and third-party mods link `../../shared/osfui.js` by that exact path;
  // padnav.js is private-but-unfrozen, shipped as-is pending in-game controller
  // verification (frontend/COMPATIBILITY.md). All three are copied, never
  // regenerated, so byte-identical is the whole spec — and the helper being
  // hand-written JavaScript with no compile step to fail loudly, this equality
  // is what keeps an edit to src/shared-kit/osfui.js from shipping beside a
  // stale copy of itself.
  const verbatim: Array<[string, string]> = [
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

  describe.each(BUILD_VIEWS)('$mod/$name/index.html', (v) => {
    const html = () => readFileSync(join(OUT, v.mod, v.name, 'index.html'), 'utf8');

    it('does not use type="module"', () => {
      // shared/osfui.js owns osfui.onMessage, defines on()/state.on(), and
      // sends the page-initiated `osfui.hello` from its own IIFE body — all of
      // it has to happen before main.js runs. Modules are deferred, which
      // silently inverts that order even if CORS passed, leaving the view
      // subscribing through members that do not exist yet.
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
