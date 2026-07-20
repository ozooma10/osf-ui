// Every emitted manifest.json must be loadable by the native ViewManifest
// parser and valid against the schema we publish.
//
// The runtime parses manifests leniently — unknown keys are allowed, since a
// manifest written for a newer OSF UI carrying extra fields is the normal
// compatible case (api-freeze-plan item 8) — but it hard-rejects two things,
// and a rejected manifest means the view never registers: no menu entry, no
// error the user can act on. Both are asserted below.

import { describe, it, expect } from 'vitest';
import { readFileSync } from 'node:fs';
import { join } from 'node:path';
import Ajv2020 from 'ajv/dist/2020';
import { OUT, REPO, VIEWS } from '../scripts/config.mjs';

// manifest.schema.json declares draft 2020-12. Ajv's default export only
// understands draft-07 and throws "no schema with key or ref .../2020-12/
// schema"; `ajv/dist/2020` is the draft-2020-12 build.
const SCHEMA_PATH = join(REPO, 'docs', 'schema', 'manifest.schema.json');
const schema = JSON.parse(readFileSync(SCHEMA_PATH, 'utf8'));

// strict:false — the schema uses "default" and other annotations that Ajv's
// strict mode flags as unknown/ineffective keywords. They document the
// published schema for humans; failing on them tests Ajv's opinions, not the
// manifests.
const ajv = new Ajv2020({ strict: false, allErrors: true });
const validate = ajv.compile(schema);

describe.each(VIEWS)('$mod/$name/manifest.json', (v) => {
  const file = join(OUT, v.mod, v.name, 'manifest.json');

  // Keep the I/O inside the `it` callbacks. Hoisting the parse to the describe
  // body puts it in vitest's collection phase, where a throw is not a test
  // failure but aborts the whole file: one malformed manifest reports as
  // "0 test" and cancels every gate below for every view. It also makes the
  // 'parses as JSON' test unfalsifiable, since the only input that could fail
  // it kills the run before the test is registered.
  const read = () => readFileSync(file, 'utf8');
  const load = () => JSON.parse(read()) as Record<string, unknown>;

  it('parses as JSON', () => {
    expect(() => JSON.parse(read())).not.toThrow();
  });

  it('has an "id" equal to its containing folder name', () => {
    const manifest = load();
    // ViewManifest hard-rejects a mismatch. The qualified view id the runtime
    // uses for menu.open / RegisterView is derived from the path
    // ("<modId>/<viewName>"), so an id that disagrees with the folder would
    // describe a view that cannot be addressed.
    expect(manifest['id']).toBe(v.name);
  });

  it('has an "entry" that is relative, ".."-free, and at the view root', () => {
    const entry = load()['entry'];
    expect(typeof entry).toBe('string');
    const e = entry as string;

    // The native loader rejects absolute paths and any '..' component — this
    // is the view-directory sandbox, not a style rule. Both separators are
    // checked: manifests are authored on Windows but the path resolves against
    // a file:// URL.
    expect(e.startsWith('/')).toBe(false);
    expect(e.startsWith('\\')).toBe(false);
    expect(/^[a-zA-Z]:/.test(e)).toBe(false); // C:\... style absolute
    expect(e.split(/[\\/]/)).not.toContain('..');

    // The entry must stay at the view root, with no directory component.
    // index.html is authored relative to the document and `../../shared/
    // osfui.js` is exactly two levels up from views/<mod>/<view>/. An entry in
    // a subfolder (e.g. "html/index.html") resolves that to
    // views/<mod>/shared/…, which does not exist: the shared kit never loads,
    // window.osfui is never wired, the view is blank. The schema cannot
    // express this.
    expect(e.split(/[\\/]/).length).toBe(1);
  });

  it('validates against docs/schema/manifest.schema.json', () => {
    const ok = validate(load());
    // Surface Ajv's message list on failure; a bare `toBe(true)` is
    // undebuggable. Ajv returns "No errors" when the error list is empty.
    expect(ajv.errorsText(validate.errors, { separator: '\n' })).toBe('No errors');
    expect(ok).toBe(true);
  });
});
