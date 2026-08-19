
import { describe, it, expect } from 'vitest';
import { readFileSync } from 'node:fs';
import { join } from 'node:path';
import { parse } from 'acorn';
import { OUT, expectedOutputs, walk } from '../scripts/config.mjs';

const jsFiles = walk(OUT).filter((f: string) => f.endsWith('.js')).sort();

describe('emitted JavaScript is a classic ES2020 script', () => {
  it('checks every .js the build claims to emit (no vacuous pass on a partial tree)', () => {
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
