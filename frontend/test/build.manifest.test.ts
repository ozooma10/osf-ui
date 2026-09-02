
import { describe, it, expect } from 'vitest';
import { readFileSync } from 'node:fs';
import { join } from 'node:path';
import Ajv2020 from 'ajv/dist/2020';
import { BUILD_VIEWS, OUT, REPO } from '../scripts/config.mjs';

const SCHEMA_PATH = join(REPO, 'docs', 'schema', 'manifest.schema.json');
const schema = JSON.parse(readFileSync(SCHEMA_PATH, 'utf8'));

const ajv = new Ajv2020({ strict: false, allErrors: true });
const validate = ajv.compile(schema);

describe('built-in view catalog', () => {
  it('may be empty in the OSF UI 2 runtime package', () => {
    expect(BUILD_VIEWS).toEqual([]);
  });

  it('requires manifestVersion 1 for third-party views', () => {
    expect(validate({ manifestVersion: 1, kind: 'hud' })).toBe(true);
    expect(validate({ kind: 'hud' })).toBe(false);
    expect(validate({ manifestVersion: 2, kind: 'hud' })).toBe(false);
  });
});

describe.each(BUILD_VIEWS)('$mod/$name/manifest.json', (v) => {
  const file = join(OUT, v.mod, v.name, 'manifest.json');

  const read = () => readFileSync(file, 'utf8');
  const load = () => JSON.parse(read()) as Record<string, unknown>;

  it('parses as JSON', () => {
    expect(() => JSON.parse(read())).not.toThrow();
  });

  it('declares no "id"', () => {
    const manifest = load();
    expect(manifest['id']).toBeUndefined();
  });

  it('has an "entry" that is relative, ".."-free, and at the view root', () => {
    const entry = load()['entry'];
    expect(typeof entry).toBe('string');
    const e = entry as string;

    expect(e.startsWith('/')).toBe(false);
    expect(e.startsWith('\\')).toBe(false);
    expect(/^[a-zA-Z]:/.test(e)).toBe(false); // C:\... style absolute
    expect(e.split(/[\\/]/)).not.toContain('..');

    expect(e.split(/[\\/]/).length).toBe(1);
  });

  it('validates against docs/schema/manifest.schema.json', () => {
    const ok = validate(load());
    expect(ajv.errorsText(validate.errors, { separator: '\n' })).toBe('No errors');
    expect(ok).toBe(true);
  });
});
