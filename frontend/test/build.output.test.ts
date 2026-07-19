// build.output.test.ts — the shipped-file-set gate.
//
// scripts/verify-output.mjs already runs inside `npm run build`, which stops a
// bad build from reaching the tree. Running it again here is not redundant: it
// turns a build-time abort (a stack trace nobody attributes) into a NAMED test
// failure in CI, and it re-checks output that may have been hand-edited after
// the build. build.mjs and this file are the only two callers.
//
// The extra assertions below duplicate a few of verifyOutput()'s checks on
// purpose. verifyOutput() returns a flat string[] of problems; if it ever
// regresses to returning [] early (a thrown-and-swallowed error, an OUT path
// that resolves nowhere), the "no problems" assertion passes vacuously. The
// direct assertions cannot pass vacuously — they name the files.

import { describe, it, expect } from 'vitest';
import { readFileSync } from 'node:fs';
import { join } from 'node:path';
import { OUT, FRONTEND, VIEWS, expectedOutputs, walk } from '../scripts/config.mjs';
import { verifyOutput } from '../scripts/verify-output.mjs';

describe('build output', () => {
  it('passes every gate in verify-output.mjs', () => {
    const problems = verifyOutput();
    // Assert on the array itself so a failure prints the problem text rather
    // than "expected 3 to be 0".
    expect(problems).toEqual([]);
  });

  it('emits exactly the expected file set', () => {
    // Equality in BOTH directions. A missing file breaks the view; an
    // unexpected one ships forever, because neither tools/package.ps1's
    // recursive staging copy nor the after_build MO2 redeploy (os.cp, which
    // overlays and never prunes) has any way to remove a file it once wrote.
    expect(walk(OUT).sort()).toEqual(expectedOutputs());
  });

  it('emits no source maps', () => {
    // Covered by the set-equality above today, but stated separately because
    // it is the one violation with a concrete distribution cost: nothing in
    // package.ps1 or CI excludes by extension, so a .map lands in the public
    // archive along with the original TSX paths embedded in it.
    expect(walk(OUT).filter((f: string) => f.endsWith('.map'))).toEqual([]);
  });

  // The frozen public contract. shared/osfui.{js,css} are bridge protocol 1.0
  // (api-freeze item 5) and third-party mods link `../../shared/osfui.js` by
  // exact path; padnav.js is private-but-unfrozen and ships as-is pending
  // in-game controller verification (see frontend/COMPATIBILITY.md). All three
  // are copied, never regenerated — so "byte-identical" is the whole spec.
  const verbatim: Array<[string, string]> = [
    ['src/shared-kit/osfui.js', 'shared/osfui.js'],
    ['src/shared-kit/osfui.css', 'shared/osfui.css'],
    ['src/legacy/padnav.js', 'osfui/padnav.js'],
  ];

  it.each(verbatim)('%s is copied byte-identically to %s', (src, out) => {
    const a = readFileSync(join(FRONTEND, src));
    const b = readFileSync(join(OUT, out));
    // Compare buffers, not strings: a UTF-8 BOM or a CRLF/LF rewrite is a real
    // byte-level change to a frozen artifact and must fail, even though both
    // sides would decode to the "same" text.
    expect(b.equals(a)).toBe(true);
  });

  describe.each(VIEWS)('$mod/$name/index.html', (v) => {
    const html = () => readFileSync(join(OUT, v.mod, v.name, 'index.html'), 'utf8');

    it('does not use type="module"', () => {
      // See build.syntax.test.ts for the full reasoning: module scripts are
      // CORS-blocked on Ultralight's opaque file:// origin and the view renders
      // blank. Load order is also load-bearing here — shared/osfui.js OWNS
      // osfui.onMessage and must execute before main.js — and modules are
      // deferred, which would silently invert that order even if CORS passed.
      expect(html()).not.toMatch(/type\s*=\s*["']module["']/);
    });

    it('has no crossorigin attribute', () => {
      // Vite's HTML pipeline injects `crossorigin` alongside `type="module"`.
      // Its presence means index.html was PROCESSED rather than copied, which
      // also rewrites hrefs against `base` and hashes assets — destroying the
      // exact `../../shared/osfui.css` relative depth that
      // docs/authoring-views.md promises third-party view authors.
      expect(html()).not.toMatch(/\bcrossorigin\b/);
    });
  });
});
