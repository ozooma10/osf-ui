// The System Health model: counting, severity precedence, sorting, per-mod
// attribution, code->copy mapping, and diagnostic-report serialization.

import { describe, it, expect } from 'vitest';
import {
  activeIssues,
  canRetryView,
  copyForIssue,
  countIssues,
  GENERIC_COPY,
  issueForSubject,
  MOD_COPY,
  modIdOf,
  overallSeverity,
  readHealth,
  resolvedIssues,
  serializeReport,
  severityForMod,
  sortIssues,
  type IssueRecord,
} from '@lib/settings/diagnostics';

const issue = (o: Partial<IssueRecord> & { id: string }): IssueRecord => ({
  code: 'view.load-failed',
  severity: 'warning',
  status: 'active',
  source: 'views',
  subject: '',
  context: {},
  occurrences: 1,
  firstAt: 0,
  lastAt: 0,
  ...o,
});

describe('readHealth', () => {
  it('normalizes an untrusted payload and drops idless issues', () => {
    const model = readHealth({
      system: { version: '1.4' },
      issues: [issue({ id: 'a' }), { code: 'x' }, null, { id: '' }],
    });
    expect(model.system).toEqual({ version: '1.4' });
    expect(model.issues.map((i) => i.id)).toEqual(['a']);
  });

  it('survives a missing or malformed payload', () => {
    expect(readHealth(undefined)).toEqual({ system: {}, issues: [] });
    expect(readHealth({ issues: 'nope' }).issues).toEqual([]);
  });
});

describe('countIssues + overallSeverity', () => {
  it('counts active only and uses error precedence', () => {
    const issues = [
      issue({ id: 'e', severity: 'error' }),
      issue({ id: 'w', severity: 'warning' }),
      issue({ id: 'r', severity: 'error', status: 'resolved' }),
    ];
    expect(countIssues(issues)).toEqual({ errors: 1, warnings: 1, resolved: 1 });
    expect(overallSeverity(countIssues(issues))).toBe('error');
  });

  it('is nominal when only resolved issues remain', () => {
    const issues = [issue({ id: 'r', severity: 'error', status: 'resolved' })];
    expect(countIssues(issues)).toEqual({ errors: 0, warnings: 0, resolved: 1 });
    expect(overallSeverity(countIssues(issues))).toBeNull();
  });

  it('a resolved error never keeps the badge red', () => {
    const model = readHealth({
      system: {},
      issues: [issue({ id: 'r', severity: 'error', status: 'resolved' })],
    });
    expect(activeIssues(model)).toHaveLength(0);
    expect(resolvedIssues(model)).toHaveLength(1);
  });
});

describe('sortIssues', () => {
  it('is errors first, then warnings, newest-first within each', () => {
    const out = sortIssues([
      issue({ id: 'w-old', severity: 'warning', lastAt: 1 }),
      issue({ id: 'e-old', severity: 'error', lastAt: 2 }),
      issue({ id: 'w-new', severity: 'warning', lastAt: 3 }),
      issue({ id: 'e-new', severity: 'error', lastAt: 4 }),
    ]);
    expect(out.map((i) => i.id)).toEqual(['e-new', 'e-old', 'w-new', 'w-old']);
  });
});

describe('severityForMod — rail marker attribution', () => {
  const issues = [
    issue({ id: '1', severity: 'warning', subject: 'acme.kit' }),
    issue({ id: '2', severity: 'error', subject: 'acme.kit/panel' }),
    issue({ id: '3', severity: 'error', subject: 'other.mod' }),
    issue({ id: '4', severity: 'error', subject: 'view:solo', status: 'resolved' }),
  ];

  it('takes the worst ACTIVE severity of issues owned by the mod or its views', () => {
    expect(severityForMod(issues, 'acme.kit')).toBe('error'); // the view failure wins
    expect(severityForMod(issues, 'other.mod')).toBe('error');
  });

  it('matches a view id passed explicitly (view-only entry)', () => {
    const solo = [issue({ id: '5', severity: 'warning', subject: 'solo/hud' })];
    expect(severityForMod(solo, 'view:solo', ['solo/hud'])).toBe('warning');
  });

  it("attributes a mod's OWN reports by source, whatever their subject", () => {
    // An ABI 1.7 report names the thing the mod cares about — a pack, a file —
    // which is never the mod id, so source is the only link back to the rail.
    const own = [issue({ id: 'r', severity: 'error', source: 'acme.kit', subject: 'highlights' })];
    expect(severityForMod(own, 'acme.kit')).toBe('error');
    expect(severityForMod(own, 'other.mod')).toBeNull();
  });

  it('ignores resolved issues and unrelated subjects', () => {
    expect(severityForMod([issues[3] as IssueRecord], 'solo')).toBeNull();
    expect(severityForMod(issues, 'nobody')).toBeNull();
  });
});

describe('issueForSubject — deep-link target', () => {
  it('finds the active issue naming a subject, preferring errors', () => {
    const issues = [
      issue({ id: 'w', severity: 'warning', subject: 'x/y', lastAt: 5 }),
      issue({ id: 'e', severity: 'error', subject: 'x/y', lastAt: 1 }),
    ];
    expect(issueForSubject(issues, 'x/y')?.id).toBe('e');
    expect(issueForSubject(issues, 'nope')).toBeNull();
  });
});

describe('copyForIssue', () => {
  it('maps known codes to copy with actions', () => {
    const copy = copyForIssue(issue({ id: 'a', code: 'view.load-failed' }));
    expect(copy.actions).toContain('retry-view');
    expect(copy.title[1]).toMatch(/could not be loaded/i);
  });

  it('falls back to generic copy for an unknown or absent code', () => {
    expect(copyForIssue(issue({ id: 'a', code: 'future.unknown', source: 'host' }))).toBe(
      GENERIC_COPY,
    );
    expect(copyForIssue(issue({ id: 'a', code: '', source: 'host' }))).toBe(GENERIC_COPY);
  });

  it('separates a mod-reported code from an unknown platform one', () => {
    // Platform source, unknown code: this build is behind -> the generic card.
    expect(copyForIssue(issue({ id: 'a', code: 'future.unknown', source: 'host' }))).toBe(
      GENERIC_COPY,
    );
    // Mod source: updating OSF UI would change nothing, so the card names the
    // mod and points at its author instead.
    const mod = copyForIssue(
      issue({ id: 'b', code: 'osf.animation:catalog.parse-failed', source: 'osf.animation' }),
    );
    expect(mod.title).toEqual(MOD_COPY.title);
    expect(mod.params).toEqual({ mod: 'osf.animation' });
    expect(mod.next[1]).not.toMatch(/update osf ui/i);
    // A code this build DOES know still wins over the fallback, whoever sent it.
    expect(copyForIssue(issue({ id: 'c', code: 'view.load-failed', source: 'acme.kit' }))).toBe(
      copyForIssue(issue({ id: 'd', code: 'view.load-failed', source: 'host' })),
    );
  });

  it('tells a mod source from a platform one by the mod-id dot', () => {
    expect(modIdOf(issue({ id: 'a', source: 'osf.animation' }))).toBe('osf.animation');
    for (const platform of ['settings', 'views', 'host', 'render', 'compat', '']) {
      expect(modIdOf(issue({ id: 'a', source: platform }))).toBeNull();
    }
  });

  it('offers Retry view only when a subject is present', () => {
    expect(canRetryView(issue({ id: 'a', code: 'view.load-failed', subject: 'x/y' }))).toBe(true);
    expect(canRetryView(issue({ id: 'a', code: 'view.load-failed', subject: '' }))).toBe(false);
    // A warning family that never offers retry stays false regardless of subject.
    expect(canRetryView(issue({ id: 'a', code: 'host.ring-truncated', subject: 'x' }))).toBe(false);
  });
});

describe('serializeReport', () => {
  it('renders system, active and resolved sections as readable text', () => {
    const model = readHealth({
      system: { version: '1.4.0', renderer: 'webview2' },
      issues: [
        issue({ id: 'e', severity: 'error', subject: 'x/y', code: 'view.load-failed', occurrences: 3 }),
        issue({ id: 'r', status: 'resolved', subject: 'a', code: 'settings.values-parse', resolvedAt: 9 }),
      ],
    });
    const text = serializeReport(model);
    expect(text).toContain('OSF UI diagnostic report');
    expect(text).toContain('version: 1.4.0');
    expect(text).toContain('Active issues (1)');
    expect(text).toContain('[error] view.load-failed — x/y');
    expect(text).toContain('occurrences=3');
    expect(text).toContain('Resolved this session (1)');
    expect(text).toContain('resolved=9s');
  });

  it('says so when there is nothing to report', () => {
    const text = serializeReport({ system: {}, issues: [] });
    expect(text).toContain('Active issues (0)');
    expect(text).toContain('none');
  });
});
