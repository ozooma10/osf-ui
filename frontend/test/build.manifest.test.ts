// build.manifest.test.ts — every emitted manifest.json must be loadable by the
// native ViewManifest parser AND valid against the schema we publish.
//
// The runtime parses manifests leniently (unknown keys are allowed by design —
// a manifest written for a newer OSF UI carrying extra fields is the normal
// compatible case, api-freeze-plan item 8), but it HARD-REJECTS two things,
// and a rejected manifest means the view never registers: no menu entry, no
// error the user can act on. Both are asserted below.

import { describe, it, expect } from 'vitest';
import { readFileSync } from 'node:fs';
import { join } from 'node:path';
import Ajv2020 from 'ajv/dist/2020';
import { OUT, REPO, VIEWS } from '../scripts/config.mjs';

// docs/schema/manifest.schema.json declares
// "$schema": "https://json-schema.org/draft/2020-12/schema".
// Ajv's DEFAULT export only understands draft-07; handing it a 2020-12 schema
// throws "no schema with key or ref .../2020-12/schema". `ajv/dist/2020` is the
// draft-2020-12 build and is the correct entry point here.
const SCHEMA_PATH = join(REPO, 'docs', 'schema', 'manifest.schema.json');
const schema = JSON.parse(readFileSync(SCHEMA_PATH, 'utf8'));

// strict:false — the schema uses "default" on properties and other annotations
// that Ajv's strict mode flags as unknown/ineffective keywords. Those are
// documentation for humans reading the published schema, not validation rules;
// failing on them would be testing Ajv's opinions, not the manifests.
const ajv = new Ajv2020({ strict: false, allErrors: true });
const validate = ajv.compile(schema);

describe.each(VIEWS)('$mod/$name/manifest.json', (v) => {
  const file = join(OUT, v.mod, v.name, 'manifest.json');

  // Read AND parse lazily, inside each test.
  //
  // This looks like needless repetition — hoisting `const manifest =
  // JSON.parse(readFileSync(file))` to the describe body would be tidier — but
  // that hoist puts the parse in vitest's COLLECTION phase, where a throw is
  // not a test failure: it aborts the whole file. A single malformed manifest
  // then reports as "0 test" and cancels every gate below for EVERY view,
  // including the views that are fine. It also makes the 'parses as JSON' test
  // unfalsifiable — the only input that could fail it kills the run before it
  // is ever registered. Keep the I/O inside the `it` callbacks.
  const read = () => readFileSync(file, 'utf8');
  const load = () => JSON.parse(read()) as Record<string, unknown>;

  it('parses as JSON', () => {
    expect(() => JSON.parse(read())).not.toThrow();
  });

  it('has an "id" equal to its containing folder name', () => {
    const manifest = load();
    // ViewManifest.cpp hard-rejects a mismatch. The qualified view id the
    // runtime uses for menu.open / RegisterView is derived from the PATH
    // ("<modId>/<viewName>"), so an id that disagrees with the folder would
    // make the manifest describe a view that cannot be addressed.
    expect(manifest['id']).toBe(v.name);
  });

  it('has an "entry" that is relative, ".."-free, and at the view root', () => {
    const entry = load()['entry'];
    expect(typeof entry).toBe('string');
    const e = entry as string;

    // Absolute paths and any '..' component are rejected outright by the
    // native loader — this is the view-directory sandbox, not a style rule.
    // Both separators are checked: the manifests are authored on Windows but
    // the path is resolved against a file:// URL.
    expect(e.startsWith('/')).toBe(false);
    expect(e.startsWith('\\')).toBe(false);
    expect(/^[a-zA-Z]:/.test(e)).toBe(false); // C:\... style absolute
    expect(e.split(/[\\/]/)).not.toContain('..');

    // Additionally: the entry must stay AT the view root, i.e. no directory
    // component at all. Everything in index.html is authored relative to the
    // document, and `../../shared/osfui.js` is exactly two levels up from
    // views/<mod>/<view>/. Moving the entry into a subfolder (e.g.
    // "html/index.html") would silently resolve that to views/<mod>/shared/…,
    // which does not exist — the shared kit never loads, window.osfui is never
    // wired, and the view is blank. The schema cannot express this, so it is
    // asserted here.
    expect(e.split(/[\\/]/).length).toBe(1);
  });

  it('validates against docs/schema/manifest.schema.json', () => {
    const ok = validate(load());
    // Surface Ajv's own message list on failure; a bare `toBe(true)` on a
    // schema failure is undebuggable.
    // Ajv returns the literal string "No errors" when the error list is empty.
    expect(ajv.errorsText(validate.errors, { separator: '\n' })).toBe('No errors');
    expect(ok).toBe(true);
  });
});
