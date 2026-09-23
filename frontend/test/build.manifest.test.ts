
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
});
