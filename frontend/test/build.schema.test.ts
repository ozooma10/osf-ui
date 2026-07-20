// Validate every settings-schema JSON in the repo against the published
// docs/schema/settings-schema.schema.json. Nothing else does: the runtime
// parses leniently and skips what it does not understand, so drift shows up
// only as a setting silently missing from the Mods surface.
//
// Files are validated against the schema, so this fails when either side
// moves. The two are a contract.

import { describe, it, expect } from 'vitest';
import { existsSync, readFileSync } from 'node:fs';
import { join } from 'node:path';
import Ajv2020 from 'ajv/dist/2020';
import { REPO } from '../scripts/config.mjs';

// The schema declares draft 2020-12, which Ajv's default (draft-07) export
// cannot compile.
const schema = JSON.parse(
  readFileSync(join(REPO, 'docs', 'schema', 'settings-schema.schema.json'), 'utf8'),
);

// strict:false: the published schema carries human-facing annotations
// ("default", descriptions on branches) that Ajv strict mode reports as
// unknown keywords. Documentation, not validation rules.
const ajv = new Ajv2020({ strict: false, allErrors: true });
const validate = ajv.compile(schema);

// Enumerated rather than globbed: a repo-root glob also sweeps
// .claude/worktrees/**, tying this test's result to unrelated concurrent work.
// data/ is otherwise build output, but data/OSFUI/settings/osfui.json is a
// hand-maintained shipped file that happens to live there; read here, never
// written.
const files = [
  // The platform's own settings card. "osfui" is the one dotless id the schema
  // permits — dotless ids are reserved for the platform.
  join(REPO, 'data', 'OSFUI', 'settings', 'osfui.json'),
  // Copy-me starter for third-party authors; exercises every v1 widget.
  join(REPO, 'examples', 'settings-only', 'yourname.mymod.json'),
  // Papyrus native-API test fixture: not shipped, but the runtime loads it
  // during testing, and a non-conforming fixture fails two layers away.
  join(REPO, 'tests', 'papyrus', 'osfui.paptest.json'),
];

describe('shipped settings-schema JSON files', () => {
  it('all exist (a renamed/moved file must update this list, not vanish silently)', () => {
    expect(files.filter((f) => !existsSync(f))).toEqual([]);
  });

  it.each(files)('%s validates against settings-schema.schema.json', (file) => {
    const doc = JSON.parse(readFileSync(file, 'utf8'));
    const ok = validate(doc);
    // Print Ajv's messages on failure; a bare boolean assertion is
    // undebuggable against a 15KB schema.
    expect(ajv.errorsText(validate.errors, { separator: '\n' })).toBe('No errors');
    expect(ok).toBe(true);
  });

  it.each(files)('%s has an "id" matching its filename stem', (file) => {
    // The schema documents the rule (id must equal the filename stem, so the
    // file is settings/<author>.<modname>.json) but JSON Schema has no view of
    // the filename. The loader keys mods by the id inside the file, so a
    // mismatch lands the settings under an id that no other file, translation
    // catalog, or Papyrus call references.
    const doc = JSON.parse(readFileSync(file, 'utf8')) as { id?: unknown };
    const stem = file.split(/[\\/]/).pop()!.replace(/\.json$/, '');
    expect(doc.id).toBe(stem);
  });
});
