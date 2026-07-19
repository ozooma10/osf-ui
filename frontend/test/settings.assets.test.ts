import { describe, it, expect } from 'vitest';
import { DEFAULT_ASSET_ROOT, modIconSrc, safeAssetSrc } from '@lib/settings/assets';

const MOD = 'acme.shipworks';

describe('safeAssetSrc — accepted paths', () => {
  it('resolves to ../../<modId>/<src>', () => {
    expect(safeAssetSrc(MOD, 'img/diagram.png')).toBe(`${DEFAULT_ASSET_ROOT}/${MOD}/img/diagram.png`);
  });

  it('allows nested folders, dots in a filename and query-ish text', () => {
    expect(safeAssetSrc(MOD, 'a/b/c.d.png')).toBe(`${DEFAULT_ASSET_ROOT}/${MOD}/a/b/c.d.png`);
    expect(safeAssetSrc(MOD, './icon.png')).toBe(`${DEFAULT_ASSET_ROOT}/${MOD}/./icon.png`);
  });
});

describe('safeAssetSrc — every rejection', () => {
  it('rejects an empty or absent src', () => {
    expect(safeAssetSrc(MOD, '')).toBeNull();
    expect(safeAssetSrc(MOD, null)).toBeNull();
    expect(safeAssetSrc(MOD, undefined)).toBeNull();
    expect(safeAssetSrc(MOD, 0)).toBeNull();
  });

  it('rejects ".." RAW', () => {
    expect(safeAssetSrc(MOD, '../secret.png')).toBeNull();
    expect(safeAssetSrc(MOD, 'a/../../b.png')).toBeNull();
    // Broader than a segment check by design.
    expect(safeAssetSrc(MOD, 'a..b.png')).toBeNull();
  });

  it('rejects ".." after DECODING', () => {
    // WebKit resolves the URL again after this check, turning these back into
    // "../" — hence the decoded pass. (The bare "%" rule below also catches
    // them; the decoded check is the belt to that braces.)
    expect(safeAssetSrc(MOD, '%2e%2e%2fsecret.png')).toBeNull();
    expect(safeAssetSrc(MOD, '%2E%2E/secret.png')).toBeNull();
  });

  it('rejects any URL scheme, case-insensitively', () => {
    expect(safeAssetSrc(MOD, 'http://evil/x.png')).toBeNull();
    expect(safeAssetSrc(MOD, 'HTTPS://evil/x.png')).toBeNull();
    expect(safeAssetSrc(MOD, 'javascript:alert(1)')).toBeNull();
    expect(safeAssetSrc(MOD, 'data:image/png;base64,AAAA')).toBeNull();
    expect(safeAssetSrc(MOD, 'file:///c:/x.png')).toBeNull();
  });

  it('rejects a leading "/" or "\\" (absolute paths)', () => {
    expect(safeAssetSrc(MOD, '/etc/passwd')).toBeNull();
    expect(safeAssetSrc(MOD, '\\\\server\\share\\x.png')).toBeNull();
  });

  it('rejects ANY percent sign, even a harmless one', () => {
    expect(safeAssetSrc(MOD, 'fifty%20percent.png')).toBeNull();
    expect(safeAssetSrc(MOD, '100%.png')).toBeNull();
  });

  it('rejects a src whose escapes fail to decode', () => {
    // decodeURIComponent throws on a lone or malformed "%".
    expect(safeAssetSrc(MOD, '%zz.png')).toBeNull();
    expect(safeAssetSrc(MOD, '%E0%A4%A.png')).toBeNull();
  });

  it('rejects a bad mod id (the id is interpolated into the path too)', () => {
    expect(safeAssetSrc('', 'a.png')).toBeNull();
    expect(safeAssetSrc(null, 'a.png')).toBeNull();
    expect(safeAssetSrc('..', 'a.png')).toBeNull();
    expect(safeAssetSrc('../other', 'a.png')).toBeNull();
    expect(safeAssetSrc('/abs', 'a.png')).toBeNull();
    expect(safeAssetSrc('\\abs', 'a.png')).toBeNull();
    expect(safeAssetSrc('http:', 'a.png')).toBeNull();
    expect(safeAssetSrc('acme%2e', 'a.png')).toBeNull();
  });

  it('checks the src decode BEFORE the mod id', () => {
    // Order matters only for the diagnostic, but the result must stay null
    // whichever rule fires first.
    expect(safeAssetSrc('..', '%zz')).toBeNull();
  });
});

describe('safeAssetSrc — injected roots (no window global)', () => {
  it('uses the default root when no map is supplied', () => {
    expect(safeAssetSrc(MOD, 'a.png')).toBe(`../../${MOD}/a.png`);
  });

  it('uses an injected root for the matching mod id', () => {
    const roots = { [MOD]: '../../../mods/shipworks/views' };
    expect(safeAssetSrc(MOD, 'a.png')).toBe(`../../${MOD}/a.png`);
    expect(safeAssetSrc(MOD, 'a.png', roots)).toBe(`../../../mods/shipworks/views/${MOD}/a.png`);
  });

  it('falls back to the default root for an unlisted or non-string entry', () => {
    expect(safeAssetSrc(MOD, 'a.png', { other: 'x' })).toBe(`../../${MOD}/a.png`);
    const junk = { [MOD]: 7 } as unknown as Record<string, string>;
    expect(safeAssetSrc(MOD, 'a.png', junk)).toBe(`../../${MOD}/a.png`);
  });

  it('still sandboxes the src when a root is injected', () => {
    expect(safeAssetSrc(MOD, '../x.png', { [MOD]: 'anywhere' })).toBeNull();
  });
});

describe('modIconSrc', () => {
  it('resolves a schema icon through the same sandbox', () => {
    expect(modIconSrc({ id: MOD, schema: { icon: 'icon.png' } })).toBe(`../../${MOD}/icon.png`);
  });

  it('returns null for a missing, non-string or rejected icon', () => {
    expect(modIconSrc({ id: MOD })).toBeNull();
    expect(modIconSrc({ id: MOD, schema: {} })).toBeNull();
    expect(modIconSrc({ id: MOD, schema: { icon: 12 } })).toBeNull();
    expect(modIconSrc({ id: MOD, schema: { icon: '../icon.png' } })).toBeNull();
    expect(modIconSrc(null)).toBeNull();
  });
});
