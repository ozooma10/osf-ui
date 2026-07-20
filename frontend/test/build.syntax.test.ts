// build.syntax.test.ts — every emitted .js must be a classic ES2020 script.
//
// The built-in view artifact contract is one deterministic IIFE bundle per
// view. Parsing with sourceType: 'script' catches a stray import/export or
// code-splitting regression in the output even if the build configuration
// still claims to emit IIFEs.
//
// ecmaVersion 2020 is the second half of the documented artifact contract.
// Raising it is allowed when intentional, but the TypeScript target, Acorn
// gate, and frontend documentation must move together.

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
