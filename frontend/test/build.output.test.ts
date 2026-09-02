
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
    expect(walk(OUT).sort()).toEqual(expectedOutputs());
  });

  it('emits no source maps', () => {
    expect(walk(OUT).filter((f: string) => f.endsWith('.map'))).toEqual([]);
  });

  it('deterministically emits only the strict 2.0 helper', () => {
    expect(readFileSync(join(OUT, 'shared/osfui.js'), 'utf8')).toBe(composeHelper());
  });

  const verbatim: Array<[string, string]> = [
    ['src/shared-kit/osfui.css', 'shared/osfui.css'],
    ['src/legacy/padnav.js', 'shared/gamepadnav.js'],
  ];

  it.each(verbatim)('%s is copied byte-identically to %s', (src, out) => {
    const a = readFileSync(join(FRONTEND, src));
    const b = readFileSync(join(OUT, out));
    expect(b.equals(a)).toBe(true);
  });

  describe.each(BUILD_VIEWS)('$mod/$name/index.html', (v) => {
    const html = () => readFileSync(join(OUT, v.mod, v.name, 'index.html'), 'utf8');

    it('does not use type="module"', () => {
      expect(html()).not.toMatch(/type\s*=\s*["']module["']/);
    });

    it('has no crossorigin attribute', () => {
      expect(html()).not.toMatch(/\bcrossorigin\b/);
    });
  });
});
