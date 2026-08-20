
import { describe, it, expect } from 'vitest';
import { existsSync, readFileSync } from 'node:fs';
import { join } from 'node:path';
import Ajv2020 from 'ajv/dist/2020';
import { REPO } from '../scripts/config.mjs';

const schema = JSON.parse(
  readFileSync(join(REPO, 'docs', 'schema', 'settings-schema.schema.json'), 'utf8'),
);

const ajv = new Ajv2020({ strict: false, allErrors: true });
const validate = ajv.compile(schema);

const files = [
  // The platform's own settings card uses the one reserved mod id.
  join(REPO, 'data', 'OSFUI', 'settings', 'osfui.json'),
  join(REPO, 'tests', 'papyrus', 'osfui.paptest.json'),
];

const osfuiSettingsFile = files[0]!;

describe('shipped settings-schema JSON files', () => {
  it('all exist (a renamed/moved file must update this list, not vanish silently)', () => {
    expect(files.filter((f) => !existsSync(f))).toEqual([]);
  });

  it.each(files)('%s validates against settings-schema.schema.json', (file) => {
    const doc = JSON.parse(readFileSync(file, 'utf8'));
    const ok = validate(doc);
    expect(ajv.errorsText(validate.errors, { separator: '\n' })).toBe('No errors');
    expect(ok).toBe(true);
  });

  it.each(files)('%s has an "id" matching its filename stem', (file) => {
    const doc = JSON.parse(readFileSync(file, 'utf8')) as { id?: unknown };
    const stem = file.split(/[\\/]/).pop()!.replace(/\.json$/, '');
    expect(doc.id).toBe(stem);
  });

  it('declares performance-sensitive developer settings as off-by-default restart latches', () => {
    const doc = JSON.parse(readFileSync(osfuiSettingsFile, 'utf8')) as {
      groups?: Array<{
        id?: string;
        collapsed?: boolean;
        settings?: Array<Record<string, unknown>>;
      }>;
    };
    const group = doc.groups?.find((candidate) => candidate.id === 'developer');
    expect(group).toMatchObject({ collapsed: true });
    const setting = group?.settings?.find((candidate) => candidate.key === 'developerMode');
    expect(setting).toMatchObject({
      type: 'bool',
      default: false,
      requires: 'restart',
    });
  });
});

describe('declarative Papyrus hotkey target schema', () => {
  const document = (setting: Record<string, unknown>) => ({
    id: 'test.hotkey',
    groups: [{ settings: [setting] }],
  });

  it('accepts an explicit GLOBAL callback target, including a namespaced script', () => {
    expect(
      validate(
        document({
          key: 'start',
          type: 'key',
          default: 'F8',
          onPress: { script: 'Acme:Hotkeys', function: 'OnHotkey' },
        }),
      ),
    ).toBe(true);
  });

  it.each([
    { key: 'start', type: 'bool', default: true, onPress: { script: 'A', function: 'B' } },
    { key: 'start', type: 'key', default: 'F8', onPress: { script: 'A' } },
    {
      key: 'start',
      type: 'key',
      default: 'F8',
      onPress: { script: 'A'.repeat(129), function: 'B' },
    },
    { key: 'start', type: 'key', default: 'F8', onPress: { script: 'A', function: 'bad-name' } },
    {
      key: 'start',
      type: 'key',
      default: 'F8',
      onPress: { script: 'A', function: 'B', args: [] },
    },
  ])('rejects an invalid target %#', (setting) => {
    expect(validate(document(setting))).toBe(false);
  });
});
