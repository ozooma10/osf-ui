
import type { DiagnosticIssue, DiagnosticsData } from '@sdk';

export type Severity = 'error' | 'warning';
export type SourceKind = 'platform' | 'mod';

/** A fully normalized `osfui/diagnostics` issue used by the renderer. */
export type IssueRecord = Omit<DiagnosticIssue, 'sourceKind'> & {
  sourceKind: SourceKind;
};

/** The `system` block, values rendered as text whatever their type. */
export type SystemInfo = DiagnosticsData['system'];

export interface HealthModel {
  system: SystemInfo;
  issues: IssueRecord[];
}

export const EMPTY_HEALTH: HealthModel = { system: {}, issues: [] };

export const HEALTH_ID = ':health';

/** Normalise an untrusted `osfui/diagnostics` state payload into the model. */
export function readHealth(payload: unknown): HealthModel {
  const p = isRecord(payload) ? payload : {};
  const rawIssues = Array.isArray(p.issues) ? p.issues : [];
  return {
    system: scalarRecord(p.system),
    issues: rawIssues.map(readIssue).filter((issue): issue is IssueRecord => issue !== null),
  };
}

function isRecord(value: unknown): value is Record<string, unknown> {
  return !!value && typeof value === 'object' && !Array.isArray(value);
}

function stringValue(value: unknown): string {
  return typeof value === 'string' ? value : '';
}

function numberValue(value: unknown): number {
  return typeof value === 'number' && Number.isFinite(value) ? value : 0;
}

function optionalNumber(value: unknown): number | undefined {
  return typeof value === 'number' && Number.isFinite(value) ? value : undefined;
}

function scalarRecord(value: unknown): Record<string, string | number | boolean> {
  if (!isRecord(value)) return {};
  const out: Record<string, string | number | boolean> = {};
  for (const [key, item] of Object.entries(value)) {
    if (
      typeof item === 'string' ||
      typeof item === 'boolean' ||
      (typeof item === 'number' && Number.isFinite(item))
    ) {
      out[key] = item;
    }
  }
  return out;
}

function readIssue(value: unknown): IssueRecord | null {
  if (!isRecord(value) || typeof value.id !== 'string' || value.id === '') return null;
  const resolvedAt = optionalNumber(value.resolvedAt);
  const issue: IssueRecord = {
    id: value.id,
    code: stringValue(value.code),
    severity: value.severity === 'error' ? 'error' : 'warning',
    status: value.status === 'resolved' ? 'resolved' : 'active',
    source: stringValue(value.source),
    sourceKind: value.sourceKind === 'mod' ? 'mod' : 'platform',
    subject: stringValue(value.subject),
    context: scalarRecord(value.context),
    occurrences: Math.max(1, Math.trunc(numberValue(value.occurrences))),
    firstAt: numberValue(value.firstAt),
    lastAt: numberValue(value.lastAt),
  };
  if (resolvedAt !== undefined) issue.resolvedAt = resolvedAt;
  return issue;
}

export function isResolved(issue: IssueRecord): boolean {
  return issue.status === 'resolved';
}

export function severityOf(issue: IssueRecord): Severity {
  return issue.severity;
}

export interface HealthCounts {
  errors: number;
  warnings: number;
  resolved: number;
}

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

export function sortIssues(issues: readonly IssueRecord[]): IssueRecord[] {
  return issues.slice().sort((a, b) => {
    const sa = severityOf(a) === 'error' ? 0 : 1;
    const sb = severityOf(b) === 'error' ? 0 : 1;
    if (sa !== sb) return sa - sb;
    return b.lastAt - a.lastAt;
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

export function severityForMod(
  issues: readonly IssueRecord[],
  modId: string,
  viewIds: readonly string[] = [],
): Severity | null {
  let worst: Severity | null = null;
  for (const issue of issues) {
    if (isResolved(issue)) continue;
    const subject = issue.subject;
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


export type ActionKind = 'retry-view' | 'copy-details';

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

const COPY: Record<string, IssueCopy> = {
  'input.control-map-unavailable': {
    title: ['issueControlMapTitle', "Starfield's key map is unavailable"],
    impact: [
      'issueControlMapImpact',
      'Game-binding rows and warnings are disabled, and mode-scoped mod hotkeys will not fire.',
    ],
    next: [
      'issueControlMapNext',
      'Update OSF UI for this Starfield version, then restart the game.',
    ],
    actions: ['copy-details'],
  },
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
    actions: ['copy-details'],
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
    actions: ['copy-details'],
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
    actions: ['copy-details'],
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
    actions: ['copy-details'],
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
    actions: ['retry-view', 'copy-details'],
  },
  'view.protocol-misuse': {
    title: ['issueProtocolMisuseTitle', 'A mod view sent invalid OSF UI messages'],
    impact: [
      'issueProtocolMisuseImpact',
      'Some actions or updates from this view may not work because it repeatedly used the OSF UI API incorrectly.',
    ],
    next: [
      'issueProtocolMisuseNext',
      'Update the mod that provides this view, or report the details below to its author.',
    ],
    actions: ['copy-details'],
  },
  'host.ring-truncated': {
    title: ['issueRingTruncatedTitle', 'The browser host does not match this OSF UI'],
    impact: [
      'issueRingTruncatedImpact',
      'Frames may be dropped, which shows up as a choppy overlay. This usually means two OSF UI installs are mixed.',
    ],
    next: [
      'issueRingTruncatedNext',
      'Make sure only one copy of OSF UI is enabled, then restart the game.',
    ],
    actions: ['copy-details'],
  },
  'compat.needs-newer-osfui': {
    title: ['issueNeedsNewerTitle', 'Something installed expects a newer OSF UI'],
    impact: [
      'issueNeedsNewerImpact',
      'It still loads, but parts of it may be missing or read-only on this version.',
    ],
    next: ['issueNeedsNewerNext', 'Update OSF UI to the version it asks for.'],
    actions: ['copy-details'],
  },
  'compat.unsupported-api': {
    title: ['issueUnsupportedAbiTitle', 'A native plugin requested an unsupported OSF UI ABI'],
    impact: [
      'issueUnsupportedAbiImpact',
      'OSF UI refused the bridge request because that plugin uses a different ABI major.',
    ],
    next: [
      'issueUnsupportedAbiNext',
      'Update the DLL named below to a version built for the OSF UI native ABI 1.x line.',
    ],
    actions: ['copy-details'],
  },
};

/** Fallback for a code this build predates. */
export const GENERIC_COPY: IssueCopy = {
  title: ['issueGenericTitle', 'A problem was reported'],
  impact: [
    'issueGenericImpact',
    'This version of OSF UI does not recognise this report. The technical details are below.',
  ],
  next: ['issueGenericNext', 'Update OSF UI, or use the details below when asking for help.'],
  actions: ['copy-details'],
};

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
  actions: ['copy-details'],
};

export function modIdOf(issue: IssueRecord): string | null {
  const source = issue.source;
  return issue.sourceKind === 'mod' && source ? source : null;
}

export function copyForIssue(issue: IssueRecord): IssueCopy {
  const exact = COPY[issue.code];
  if (exact) return exact;
  const mod = modIdOf(issue);
  return mod ? { ...MOD_COPY, params: { mod } } : GENERIC_COPY;
}

/** True when the card should offer "Retry view" for this issue. */
export function canRetryView(issue: IssueRecord): boolean {
  return copyForIssue(issue).actions.indexOf('retry-view') >= 0 && !!issue.subject;
}
