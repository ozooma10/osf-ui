// Every emitted .js must be a classic ES2020 script.
//
// The view artifact contract is one deterministic IIFE bundle per view. Parsing
// with sourceType: 'script' catches a stray import/export or code-splitting
// regression even when the build config still claims to emit IIFEs. ecmaVersion
// 2020 is the other half of the contract; raising it means moving the TypeScript
// target, this Acorn gate, and the frontend docs together.

import { describe, it, expect } from 'vitest';
import { readFileSync } from 'node:fs';
import { join } from 'node:path';
import { parse } from 'acorn';
import { OUT, expectedOutputs, walk } from '../scripts/config.mjs';

const jsFiles = walk(OUT).filter((f: string) => f.endsWith('.js')).sort();

describe('emitted JavaScript is a classic ES2020 script', () => {
  it('checks every .js the build claims to emit (no vacuous pass on a partial tree)', () => {
    // The parse cases below come from what is on disk, so an unbuilt or
    // half-built tree yields few or no cases and goes green having proved
    // nothing. `length > 0` is not enough — a single stale .js passes it.
    // Compare against the build's own manifest so a missing bundle fails here.
    // Set equality, not a count: same-size-different-set is the drift a count
    // misses.
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
