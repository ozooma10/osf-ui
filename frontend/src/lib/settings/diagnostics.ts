// The System Health model: what the `diagnostics.data` snapshot means, and how
// a stable machine code becomes something a player can act on.
//
// The split is deliberate. Native decides WHAT is wrong — it owns the stable
// `code`, the severity, and the bounded technical context. This file decides how
// that READS and what buttons it offers, because copy has to be localizable and
// because the set of actions the shell is willing to expose must be a closed
// list here rather than anything a payload can name. A code this build has never
// heard of still renders: it degrades to a generic card with its technical
// details shown, never to a blank one.
//
// Records are typed loosely (every field optional but `id`) for the same reason
// the rail model is: this also runs against harness mocks and, in principle,
// against a host newer than the frontend.

import type { DiagnosticIssue, DiagnosticsData } from '@sdk';

/** A `diagnostics.data` issue as the renderer actually treats it. */
export type IssueRecord = Partial<DiagnosticIssue> & { id: string };

export type Severity = 'error' | 'warning';

/** The `system` block, values rendered as text whatever their type. */
export type SystemInfo = Partial<DiagnosticsData>['system'];

export interface HealthModel {
  system: SystemInfo;
  issues: IssueRecord[];
}

export const EMPTY_HEALTH: HealthModel = { system: {}, issues: [] };

/**
 * The Health destination's rail id. Same "~" trick as HOME_ID: native mod ids
 * can never start with it, so no mod can shadow the pinned entry.
 */
export const HEALTH_ID = '~health';

/** Normalise an untrusted `diagnostics.data` payload into the model. */
export function readHealth(payload: unknown): HealthModel {
  const p = (payload || {}) as Partial<DiagnosticsData>;
  const issues = Array.isArray(p.issues) ? (p.issues as IssueRecord[]) : [];
  return {
    system: p.system && typeof p.system === 'object' ? p.system : {},
    // A record with no id has no identity to key, sort or deep-link by.
    issues: issues.filter((i) => i && typeof i.id === 'string' && i.id !== ''),
  };
}

export function isResolved(issue: IssueRecord): boolean {
  return issue.status === 'resolved';
}

export function severityOf(issue: IssueRecord): Severity {
  return issue.severity === 'error' ? 'error' : 'warning';
}

export interface HealthCounts {
  errors: number;
  warnings: number;
  resolved: number;
}

/**
 * Counts for the badge and the summary header. ACTIVE ONLY for errors and
 * warnings — a resolved error is history, and counting it would leave the rail
 * badge red long after the condition cleared.
 */
export function countIssues(issues: readonly IssueRecord[]): HealthCounts {
  let errors = 0;
  let warnings = 0;
  let resolved = 0;
  for (const issue of issues) {
    if (isResolved(issue)) {
      resolved++;
    } else if (severityOf(issue) === 'error') {
      errors++;
    } else {
      warnings++;
    }
  }
  return { errors, warnings, resolved };
}

/** Error precedence: any active error outranks any number of warnings. */
export function overallSeverity(counts: HealthCounts): Severity | null {
  if (counts.errors > 0) return 'error';
  if (counts.warnings > 0) return 'warning';
  return null;
}

/**
 * Active issues in paint order: errors first, then warnings, newest first
 * within each. Mirrors the order native already emits, but the view must not
 * depend on that — a host is free to reorder an additive payload.
 */
export function sortIssues(issues: readonly IssueRecord[]): IssueRecord[] {
  return issues.slice().sort((a, b) => {
    const sa = severityOf(a) === 'error' ? 0 : 1;
    const sb = severityOf(b) === 'error' ? 0 : 1;
    if (sa !== sb) return sa - sb;
    return (b.lastAt ?? 0) - (a.lastAt ?? 0);
  });
}

/** Resolved issues, most recently resolved first. */
export function sortResolved(issues: readonly IssueRecord[]): IssueRecord[] {
  return issues.slice().sort((a, b) => (b.resolvedAt ?? 0) - (a.resolvedAt ?? 0));
}

export function activeIssues(model: HealthModel): IssueRecord[] {
  return sortIssues(model.issues.filter((i) => !isResolved(i)));
}

export function resolvedIssues(model: HealthModel): IssueRecord[] {
  return sortResolved(model.issues.filter(isResolved));
}

/**
 * The worst ACTIVE severity attributable to one mod, for the rail's severity
 * marker. An issue belongs to a mod when its `subject` is that mod id or a view
 * id owned by it ("<modId>/<viewName>", the manifest id grammar).
 */
export function severityForMod(
  issues: readonly IssueRecord[],
  modId: string,
  viewIds: readonly string[] = [],
): Severity | null {
  let worst: Severity | null = null;
  for (const issue of issues) {
    if (isResolved(issue)) continue;
    const subject = issue.subject || '';
    // `source` is the authority for a mod's OWN reports (ABI 1.7): those name
    // whatever the mod cares about as the subject — a pack, a file, an actor —
    // so subject-matching alone would leave them off that mod's rail marker.
    const mine =
      issue.source === modId ||
      (!!subject &&
        (subject === modId ||
          subject.startsWith(modId + '/') ||
          (issue.code === 'settings.hotkey-target' && subject.startsWith(modId + '.')) ||
          viewIds.indexOf(subject) >= 0));
    if (!mine) continue;
    if (severityOf(issue) === 'error') return 'error';
    worst = 'warning';
  }
  return worst;
}

/** The active issue naming this subject, or null — the failed-view deep link. */
export function issueForSubject(
  issues: readonly IssueRecord[],
  subject: string,
): IssueRecord | null {
  const mine = sortIssues(issues.filter((i) => !isResolved(i) && i.subject === subject));
  return mine[0] ?? null;
}

// ---------------------------------------------------------------------------
// Codes -> copy and actions
// ---------------------------------------------------------------------------

/**
 * Actions a card may offer. A CLOSED list on purpose: everything here maps to a
 * fixed, payload-free or self-derived shell operation, so no diagnostic payload
 * can name a target. `retry-view` is the one that takes an argument, and it
 * takes it from the issue's own `subject` (a view id the runtime already knows),
 * never from free text.
 */
export type ActionKind = 'retry-view' | 'update-osfui' | 'open-logs' | 'copy-details';

export interface IssueCopy {
  /** Substitutions for the three strings below, e.g. `{ mod: 'osf.animation' }`. */
  params?: Record<string, string>;
  /** i18n address suffix and the authored English, for `tr(address, english)`. */
  title: [string, string];
  /** What this means for the player, in plain language. */
  impact: [string, string];
  /** The recommended next step. */
  next: [string, string];
  actions: ActionKind[];
}

/**
 * Copy and offered actions per stable code. Adding a code here is how a new
 * native producer becomes legible; until then it renders through
 * {@link GENERIC_COPY} with its technical details visible, which is a worse
 * card but never a broken one.
 */
const COPY: Record<string, IssueCopy> = {
  'settings.schema-name': {
    title: ['issueSchemaNameTitle', 'A settings file has an unusable name'],
    impact: [
      'issueSchemaNameImpact',
      "The file was skipped, so that mod's settings do not appear here.",
    ],
    next: [
      'issueSchemaNameNext',
      'This is for the mod author to fix — report it with the details below.',
    ],
    actions: ['copy-details', 'open-logs'],
  },
  'settings.schema-parse': {
    title: ['issueSchemaParseTitle', 'A settings file could not be read'],
    impact: [
      'issueSchemaParseImpact',
      "The file was skipped, so that mod's settings do not appear here.",
    ],
    next: [
      'issueSchemaParseNext',
      'Reinstall the mod, or report the details below to its author.',
    ],
    actions: ['copy-details', 'open-logs'],
  },
  'settings.values-parse': {
    title: ['issueValuesParseTitle', 'Saved settings could not be read'],
    impact: [
      'issueValuesParseImpact',
      'This mod is running on its default settings. Your old file was kept next to it, renamed with a .bad extension.',
    ],
    next: [
      'issueValuesParseNext',
      'Set the options you want again — they will save normally from now on.',
    ],
    actions: ['copy-details', 'open-logs'],
  },
  'settings.hotkey-target': {
    title: ['issueHotkeyTargetTitle', 'A mod hotkey action is unavailable'],
    impact: [
      'issueHotkeyTargetImpact',
      'The key still fires its normal hotkey events, but the configured Papyrus function was not called.',
    ],
    next: [
      'issueHotkeyTargetNext',
      'Check that the PEX is installed and the named function exists, is GLOBAL, and accepts exactly two strings. A VM-unavailable detail can also mean the game is not ready for script dispatch.',
    ],
    actions: ['copy-details', 'open-logs'],
  },
  'view.load-retrying': {
    title: ['issueViewRetryingTitle', 'A screen failed to load and is being retried'],
    impact: [
      'issueViewRetryingImpact',
      'It cannot be opened until it loads. OSF UI is retrying automatically.',
    ],
    next: ['issueViewRetryingNext', 'Wait a moment — no action is needed yet.'],
    actions: ['copy-details'],
  },
  'view.load-failed': {
    title: ['issueViewFailedTitle', 'A screen could not be loaded'],
    impact: [
      'issueViewFailedImpact',
      'Retries were exhausted, so this screen is unavailable for the rest of this session.',
    ],
    next: [
      'issueViewFailedNext',
      'Try it again. If it keeps failing, reinstall the mod that provides it.',
    ],
    actions: ['retry-view', 'copy-details', 'open-logs'],
  },
  // NOTE: there is deliberately no `host.focus-stranded` entry. The renderer no
  // longer reports that condition — the focus watchdog corrects it within a tick
  // or two, so the card described an internal mechanism the player cannot act on
  // and which had usually already cleared by the time they read it. It stays a
  // log-only WARN. A host older than this build that still emits the code falls
  // through to GENERIC_COPY, which is the intended degradation.
  'host.ring-truncated': {
    title: ['issueRingTruncatedTitle', 'The browser helper does not match this OSF UI'],
    impact: [
      'issueRingTruncatedImpact',
      'Frames may be dropped, which shows up as a choppy overlay. This usually means two OSF UI installs are mixed.',
    ],
    next: [
      'issueRingTruncatedNext',
      'Make sure only one copy of OSF UI is enabled, then restart the game.',
    ],
    actions: ['copy-details', 'open-logs'],
  },
  // NOTE: there is deliberately no `render.framegen-fallback` entry either, and
  // for a sharper reason than the one above: that card could only ever be ACTIVE
  // in the exact state where the overlay suspends its draws, so the pane meant to
  // show it was itself invisible. See the note in src/runtime/Runtime.h.
  'compat.needs-newer-osfui': {
    title: ['issueNeedsNewerTitle', 'Something installed expects a newer OSF UI'],
    impact: [
      'issueNeedsNewerImpact',
      'It still loads, but parts of it may be missing or read-only on this version.',
    ],
    next: ['issueNeedsNewerNext', 'Update OSF UI to the version it asks for.'],
    actions: ['update-osfui', 'copy-details'],
  },
};

/** Fallback for a code this build predates. */
export const GENERIC_COPY: IssueCopy = {
  title: ['issueGenericTitle', 'A problem was reported'],
  impact: [
    'issueGenericImpact',
    'This version of OSF UI does not recognise this report. The technical details are below.',
  ],
  next: ['issueGenericNext', 'Update OSF UI, or include the details below in a bug report.'],
  actions: ['copy-details', 'open-logs'],
};

/**
 * Fallback for a condition another mod reported through the ABI (1.7). It is
 * separate from {@link GENERIC_COPY} because the honest next step is different:
 * an unknown PLATFORM code means this build is behind and updating may help,
 * whereas an unknown MOD code means the report is simply not one OSF UI knows —
 * updating OSF UI would change nothing, and the mod's author is the right
 * destination. The mod is named rather than quoted: `{mod}` is substituted with
 * the id from `source`, which is host-assigned from the calling plugin, never a
 * payload field, so no mod can author the words on its own card.
 */
export const MOD_COPY: IssueCopy = {
  title: ['issueModTitle', '{mod} reported a problem'],
  impact: [
    'issueModImpact',
    'That mod flagged this itself, so part of it may not be working. The technical details are below.',
  ],
  next: [
    'issueModNext',
    "This is for that mod to fix — include the details below when you report it to its author.",
  ],
  actions: ['copy-details', 'open-logs'],
};

/**
 * The mod that reported this issue, or null when it came from OSF UI itself.
 * Platform sources are bare words ("settings", "views", "host", "render",
 * "compat"); a consumer's source is its "<author>.<modname>" id, which the host
 * validated before accepting the report — so the dot is a reliable tell.
 */
export function modIdOf(issue: IssueRecord): string | null {
  const source = issue.source || '';
  return source.indexOf('.') > 0 ? source : null;
}

/**
 * Copy for one issue: an exact code match wins, then the mod-reported fallback,
 * then the platform generic.
 *
 * This is the only entry point. A bare code->copy lookup used to sit beside it
 * and could not distinguish "this build does not know that code" from "a mod
 * reported it", which is the difference between telling the player to update
 * OSF UI and telling them to contact the mod author.
 */
export function copyForIssue(issue: IssueRecord): IssueCopy {
  const exact = issue.code ? COPY[issue.code] : undefined;
  if (exact) return exact;
  const mod = modIdOf(issue);
  return mod ? { ...MOD_COPY, params: { mod } } : GENERIC_COPY;
}

/** True when the card should offer "Retry view" for this issue. */
export function canRetryView(issue: IssueRecord): boolean {
  return copyForIssue(issue).actions.indexOf('retry-view') >= 0 && !!issue.subject;
}

// ---------------------------------------------------------------------------
// Diagnostic report
// ---------------------------------------------------------------------------

/**
 * The "Copy diagnostic report" text. Plain text rather than JSON: it is pasted
 * into a forum post or a bug report, and it has to stay readable there. Only
 * what the payload already carries goes in — the payload is pre-redacted
 * natively, so this cannot leak a path the pane does not already show.
 */
export function serializeReport(model: HealthModel): string {
  const lines: string[] = ['OSF UI diagnostic report', ''];

  lines.push('System');
  const system = model.system || {};
  const keys = Object.keys(system);
  if (!keys.length) {
    lines.push('  (unavailable)');
  } else {
    for (const key of keys) lines.push(`  ${key}: ${String(system[key])}`);
  }

  const active = activeIssues(model);
  const resolved = resolvedIssues(model);

  lines.push('', `Active issues (${active.length})`);
  if (!active.length) lines.push('  none');
  for (const issue of active) lines.push(...reportIssue(issue));

  lines.push('', `Resolved this session (${resolved.length})`);
  if (!resolved.length) lines.push('  none');
  for (const issue of resolved) lines.push(...reportIssue(issue));

  return lines.join('\n');
}

function reportIssue(issue: IssueRecord): string[] {
  const lines = [
    `  [${severityOf(issue)}] ${issue.code || '(no code)'}` +
      (issue.subject ? ` — ${issue.subject}` : ''),
    `    id=${issue.id} occurrences=${issue.occurrences ?? 1}` +
      ` first=${issue.firstAt ?? 0}s last=${issue.lastAt ?? 0}s` +
      (isResolved(issue) ? ` resolved=${issue.resolvedAt ?? 0}s` : ''),
  ];
  const context = issue.context || {};
  for (const key of Object.keys(context)) {
    lines.push(`    ${key}: ${String(context[key])}`);
  }
  return lines;
}
