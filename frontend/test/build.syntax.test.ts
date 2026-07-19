// build.syntax.test.ts — every emitted .js must be a classic ES2020 script.
//
// WHY this gate exists (and why it is not merely a lint preference):
//
//   Ultralight loads views from disk over file:///. A file:// document has an
//   OPAQUE origin, and module scripts (`<script type="module">`, and anything
//   containing a top-level `import`/`export`) are fetched with CORS semantics.
//   An opaque origin fails that check unconditionally, so the module never
//   executes. There is no error dialog and no crash: the view simply renders
//   BLANK over the game, and the only trace is a console message in a devtools
//   surface that does not exist in a shipped build. That failure mode is
//   invisible to CI unless something asserts the shape of the emitted code.
//
//   Parsing with sourceType: 'script' is the assertion. A stray `import` or
//   `export` anywhere in a bundle is a syntax error in script mode, so this
//   parse succeeds if and only if the bundles are genuinely classic IIFE.
//   scripts/build.mjs asks Rollup for `format: 'iife'`, but a mis-set option, a
//   plugin that re-injects a chunk import, or a hand-edited file would all slip
//   past that intent — hence checking the OUTPUT, not the config.
//
//   ecmaVersion 2020 is the second half of the gate. It is the highest level
//   with in-repo proof that the engine actually runs it (see the target comment
//   in frontend/tsconfig.json: ES2019 optional-catch-binding and ES2020 `??`
//   both reach DOM ready under Ultralight with no console errors). ES2021+
//   syntax — `??=`, `||=`, `&&=`, numeric separators, `.at()`-era downlevel
//   assumptions — has never been demonstrated on Ultralight's JavaScriptCore
//   fork, and a parse error there is again a blank view. So we pin the parser
//   to the version we have evidence for and let anything newer fail here, in
//   CI, instead of in game.

import { describe, it, expect } from 'vitest';
import { readFileSync } from 'node:fs';
import { join } from 'node:path';
import { parse } from 'acorn';
import { OUT, expectedOutputs, walk } from '../scripts/config.mjs';

const jsFiles = walk(OUT).filter((f: string) => f.endsWith('.js')).sort();

describe('emitted JavaScript is a classic ES2020 script', () => {
  it('checks every .js the build claims to emit (no vacuous pass on a partial tree)', () => {
    // The parse cases below are generated from what is ON DISK, so an unbuilt —
    // or half-built — tree produces few or no cases and the suite goes green
    // having proved nothing. A bare `length > 0` guard is not enough: it passes
    // on a tree holding a single stale .js. Compare against the build's own
    // manifest instead, so a MISSING bundle fails here rather than only in
    // build.output.test.ts. (Set equality, not a count: same-size-different-set
    // is exactly the drift a count misses.)
    const expected = expectedOutputs().filter((f: string) => f.endsWith('.js')).sort();
    expect(jsFiles).toEqual(expected);
    expect(expected.length).toBeGreaterThan(0);
  });

  it.each(jsFiles)('%s parses as {ecmaVersion: 2020, sourceType: "script"}', (rel) => {
    const src = readFileSync(join(OUT, rel), 'utf8');
    expect(() => {
      parse(src, { ecmaVersion: 2020, sourceType: 'script' });
    }).not.toThrow();
  });
});
