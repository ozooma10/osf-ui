import { describe, it, expect } from 'vitest';
import { deriveNeedsUpdate, versionLess, type VersionTarget } from '@lib/version';

describe('versionLess', () => {
  it('compares component-wise, numerically', () => {
    expect(versionLess('1.0.0', '1.0.1')).toBe(true);
    expect(versionLess('1.0.1', '1.0.0')).toBe(false);
    expect(versionLess('1.2.0', '2.0.0')).toBe(true);
    expect(versionLess('0.9.9', '1.0.0')).toBe(true);
  });

  it('is strict: equal versions are not "less"', () => {
    expect(versionLess('1.0.0', '1.0.0')).toBe(false);
  });

  it('compares numerically, NOT lexically', () => {
    expect(versionLess('1.2', '1.10.0')).toBe(true);
    expect(versionLess('1.10.0', '1.2')).toBe(false);
    expect(versionLess('1.0.9', '1.0.10')).toBe(true);
  });

  it('treats missing segments as 0 (unequal segment counts)', () => {
    expect(versionLess('1', '1.0.0')).toBe(false);
    expect(versionLess('1.0.0', '1')).toBe(false);
    expect(versionLess('1', '1.0.1')).toBe(true);
    expect(versionLess('1.1', '1.1.0')).toBe(false);
    expect(versionLess('', '0.0.0')).toBe(false);
    expect(versionLess('', '0.0.1')).toBe(true);
  });

  it('ignores a FOURTH component entirely', () => {
    // Only three components are compared, so a build-number suffix can't
    // make one version newer than another.
    expect(versionLess('1.0.0.1', '1.0.0.9')).toBe(false);
    expect(versionLess('1.0.0', '1.0.0.9')).toBe(false);
  });

  it('ignores trailing junk in a component (parseInt)', () => {
    // Keeps the dev harness's "1.0.0-mock" comparing sanely.
    expect(versionLess('1.0.0-mock', '1.0.0')).toBe(false);
    expect(versionLess('1.0.0-mock', '1.0.1')).toBe(true);
    expect(versionLess('1.0.0', '1.1.0-beta')).toBe(true);
  });

  it('reads a non-numeric component as 0', () => {
    expect(versionLess('1.x.0', '1.0.0')).toBe(false);
    expect(versionLess('1.x.0', '1.1.0')).toBe(true);
  });
});

describe('deriveNeedsUpdate', () => {
  const view = (label: string, targetVersion?: string): VersionTarget =>
    targetVersion === undefined ? { label } : { label, targetVersion };

  it('is quiet when nothing wants a newer host', () => {
    const out = deriveNeedsUpdate('1.0.0', [view('Atlas', '1.0.0')], [view('OSF UI', '0.9.0')]);
    expect(out).toEqual({ outdated: false, wanting: [] });
  });

  it('names every asker, views before mods', () => {
    const out = deriveNeedsUpdate(
      '1.0.0',
      [view('Star Atlas', '1.1.0'), view('Ship Almanac', '1.0.0')],
      [view('Shipworks', '2.0.0'), view('Quiet Mod', '1.0.0')],
    );
    expect(out.outdated).toBe(true);
    expect(out.wanting).toEqual(['Star Atlas', 'Shipworks']);
  });

  it('de-duplicates a label that asks from both a view and its mod', () => {
    const out = deriveNeedsUpdate('1.0.0', [view('Shipworks', '1.2.0')], [view('Shipworks', '1.3.0')]);
    expect(out.wanting).toEqual(['Shipworks']);
  });

  it('ignores undeclared and empty targetVersions', () => {
    const out = deriveNeedsUpdate('1.0.0', [view('No target'), view('Empty', '')], []);
    expect(out).toEqual({ outdated: false, wanting: [] });
  });

  it('suppresses the badge entirely when the host version is unknown', () => {
    // Pre-handshake state shows no badge even though versionLess("", "1.2.0")
    // is true. Re-derived once runtime.ready lands.
    expect(versionLess('', '1.2.0')).toBe(true);
    expect(deriveNeedsUpdate('', [view('Star Atlas', '1.2.0')], [])).toEqual({
      outdated: false,
      wanting: [],
    });
  });

  it('counts a hub:false utility view — the caller passes the UNFILTERED catalog', () => {
    const out = deriveNeedsUpdate('1.0.0', [view('Hidden helper', '1.4.0')], []);
    expect(out).toEqual({ outdated: true, wanting: ['Hidden helper'] });
  });
});
