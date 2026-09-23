
import { describe, it, expect } from 'vitest';
import { readFileSync } from 'node:fs';
import { join } from 'node:path';
import Ajv2020 from 'ajv/dist/2020';
import { REPO } from '../scripts/config.mjs';

const SCHEMA_PATH = join(REPO, 'docs', 'schema', 'manifest.schema.json');
const schema = JSON.parse(readFileSync(SCHEMA_PATH, 'utf8'));

const ajv = new Ajv2020({ strict: false, allErrors: true });
const validate = ajv.compile(schema);

describe('manifest schema', () => {
  it('requires manifestVersion 1 for third-party views', () => {
    expect(validate({ manifestVersion: 1, kind: 'hud' })).toBe(true);
    expect(validate({ kind: 'hud' })).toBe(false);
    expect(validate({ manifestVersion: 2, kind: 'hud' })).toBe(false);
  });

  it('requires a precise world texture signature and bounded browser dimensions', () => {
    expect(validate({ manifestVersion: 1, kind: 'world', placeholderSize: 1000 })).toBe(true);
    expect(validate({ manifestVersion: 1, kind: 'world' })).toBe(false);
    for (const placeholderSize of [null, '1000', 1000.5, -1, 255, 256, 512, 1024, 2048, 4096, 4097]) {
      expect(validate({ manifestVersion: 1, kind: 'world', placeholderSize })).toBe(false);
    }
    expect(validate({ manifestVersion: 1, kind: 'world', placeholderSize: 1000, width: 4096, height: 1 })).toBe(true);
    expect(validate({ manifestVersion: 1, kind: 'world', placeholderSize: 1000, width: 4097 })).toBe(false);
    expect(validate({ manifestVersion: 1, kind: 'world', placeholderSize: 1000, height: 0 })).toBe(false);
  });
});
