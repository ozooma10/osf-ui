// System Health — the pane behind the pinned rail destination.
//
// The promise this pane makes is that everything on it is true right now and
// worth a player's attention. It is not a log viewer: nothing lands here that
// a subsystem did not deliberately raise, and nothing stays here once that
// subsystem withdraws it. That is why there is no dismiss button — a card you
// could dismiss would be a card that could lie.
//
// Space follows severity. An error keeps a full card — title, what it means for
// you, what to do, and its actions all visible. An active warning collapses to a
// single row that expands in place, because a warning that is not worth acting
// on should not cost the same vertical space as one that is. Both live in the
// same #health-active list so paint order (errors first) is unchanged and a
// deep link still finds its issue either way.
//
// Severity is carried three ways (word, colour, icon), never by colour alone.
// Everything a native payload supplies is rendered as a text child; the only
// prose in the pane comes from the code->copy table in @lib/settings/diagnostics,
// so it is localizable and cannot be authored by a mod. Raw native text appears
// only under a card's collapsed technical disclosure, where it reads as
// developer detail rather than as UI chrome.

import { useEffect, useState } from 'preact/hooks';
import type { Translator } from '@lib/i18n';
import { NEXUS_PAGE_URL } from '@lib/links';
import {
  activeIssues,
  canRetryView,
  copyForIssue,
  countIssues,
  isResolved,
  overallSeverity,
  resolvedIssues,
  serializeReport,
  severityOf,
  type ActionKind,
  type HealthModel,
  type IssueRecord,
  type Severity,
} from '@lib/settings/diagnostics';


export interface HealthProps {
  health: HealthModel;
  tr: Translator;
  /** Issue id to expand and scroll to on mount (deep link from a failed card). */
  focusIssueId: string | null;
  /** menu.open on a view id — the only action that takes an argument. */
  onRetryView: (viewId: string) => void;
  /** Fire a payload-free shell command (osfui.openLogFolder, osfui.openModPage). */
  onShellCommand: (command: string) => void;
  onGetReportStatus: () => Promise<ReportStatus>;
  onSubmitReport: (report: ReportSubmission) => Promise<ReportResult>;
  onOpenReportIssue: (issueNumber: number) => void;
  onToast: (message: string, kind?: 'warn' | 'danger') => void;
}

export interface ReportStatus {
  enabled: boolean;
  logs: string[];
  retentionDays: number;
}

export interface ReportSubmission {
  title: string;
  description: string;
  reproduction: string;
}

export interface ReportResult {
  ok: boolean;
  code?: string;
  message?: string;
  reportId?: string;
  issueNumber?: number;
}

export function Health({
  health,
  tr,
  focusIssueId,
  onRetryView,
  onShellCommand,
  onGetReportStatus,
  onSubmitReport,
  onOpenReportIssue,
  onToast,
}: HealthProps) {
  const active = activeIssues(health);
  // Already sorted errors-first by activeIssues; partitioning preserves that.
  const activeErrors = active.filter((i) => severityOf(i) === 'error');
  const activeWarnings = active.filter((i) => severityOf(i) !== 'error');
  const resolved = resolvedIssues(health);
  const counts = countIssues(health.issues);
  const overall = overallSeverity(counts);
  const [historyOpen, setHistoryOpen] = useState(false);
  const [reportOpen, setReportOpen] = useState(false);
  const [reportChecking, setReportChecking] = useState(false);
  const [reportStatus, setReportStatus] = useState<ReportStatus | null>(null);
  const [reportTitle, setReportTitle] = useState('');
  const [reportDescription, setReportDescription] = useState('');
  const [reportReproduction, setReportReproduction] = useState('');
  const [reportConsent, setReportConsent] = useState(false);
  const [reportSubmitting, setReportSubmitting] = useState(false);
  const [reportResult, setReportResult] = useState<ReportResult | null>(null);

  /**
   * A deep-linked issue that now sits in the history block. The card's own
   * `defaultOpen` is not enough on its own — it would expand inside a container
   * that is still collapsed, leaving the link pointing at a pane with nothing
   * visibly different about it.
   *
   * This is reachable while the pane is open, not only at mount: you follow a
   * failed view's card here, the condition clears, and the next
   * `diagnostics.data` push moves that exact card from the active list into the
   * history. Seeding the initial state would not have covered that, which is why
   * it is an effect. Only the transition into `true` is forced, so closing the
   * disclosure by hand afterwards sticks.
   */
  const resolvedTarget = !!focusIssueId && resolved.some((i) => i.id === focusIssueId);
  useEffect(() => {
    if (resolvedTarget) setHistoryOpen(true);
  }, [resolvedTarget]);

  // Bring a deep-linked issue on screen. Expanding it is not enough: the
  // summary, the action bar, the error tier and the warning-tier header all sit
  // above the list, so a linked warning routinely opens below the fold and the
  // jump reads as "nothing happened". Same shape as the settings pane's
  // search-jump scroll (App.tsx) — the card is found by walking
  // `.health-card[data-issue]` and comparing the attribute rather than building
  // a selector string, so an id carrying a quote or bracket needs no escaping,
  // and `scrollIntoView` is guarded because jsdom omits it.
  useEffect(() => {
    if (!focusIssueId) return;
    // `historyOpen` has no other use here; reading it is what re-runs the scroll
    // once a resolved target is actually in the DOM. It is seeded open above, so
    // this covers the toggle-it-open-afterwards case.
    void historyOpen;
    const cards = document.querySelectorAll('.health-card[data-issue]');
    for (let i = 0; i < cards.length; i++) {
      const card = cards[i];
      if (card instanceof HTMLElement && card.getAttribute('data-issue') === focusIssueId) {
        if (card.scrollIntoView) card.scrollIntoView({ block: 'center' });
        return;
      }
    }
  }, [focusIssueId, historyOpen]);

  const beginReport = async () => {
    setReportChecking(true);
    try {
      const status = await onGetReportStatus();
      if (!status.enabled) {
        onToast(tr('reportingUnavailable', 'Bug reporting is turned off in OSF UI settings (Diagnostics).'), 'warn');
        return;
      }
      setReportStatus(status);
      setReportResult(null);
      // Every field starts empty and consent starts unticked on each open.
      // Consent authorizes ONE upload of this free text, which the privacy copy
      // below warns may become a public GitHub issue — a box left ticked from a
      // previous report would silently pre-authorize the next one, and a
      // pre-filled title/description already satisfies Submit's enable
      // condition. Nothing else clears these: not the ok branch, not Cancel.
      setReportTitle('');
      setReportDescription('');
      setReportReproduction('');
      setReportConsent(false);
      setReportOpen(true);
    } catch {
      onToast(tr('reportStatusFailed', 'Could not start the bug reporter.'), 'danger');
    } finally {
      setReportChecking(false);
    }
  };

  const submitReport = async () => {
    if (!reportConsent || !reportTitle.trim() || !reportDescription.trim()) return;
    setReportSubmitting(true);
    try {
      const result = await onSubmitReport({
        title: reportTitle.trim(),
        description: reportDescription.trim(),
        reproduction: reportReproduction.trim(),
      });
      setReportResult(result);
      if (result.ok) {
        onToast(tr('bugReportSubmitted', 'Bug report submitted'));
      } else {
        onToast(tr('bugReportFailed', 'Could not submit the bug report.'), 'danger');
      }
    } catch {
      setReportResult({ ok: false });
      onToast(tr('bugReportFailed', 'Could not submit the bug report.'), 'danger');
    } finally {
      setReportSubmitting(false);
    }
  };

  // Clipboard failures must not look like nothing happened, and must not take
  // the technical details away from someone who now has to transcribe them by
  // hand — so the disclosure stays open and selectable and only a toast fires.
  const copyText = (text: string, okMessage: string) => {
    const clipboard = navigator.clipboard;
    if (!clipboard || !clipboard.writeText) {
      onToast(tr('copyUnavailable', 'Copying is unavailable — select the details and copy them manually.'), 'warn');
      return;
    }
    clipboard.writeText(text).then(
      () => onToast(okMessage),
      () =>
        onToast(
          tr('copyFailed', 'Could not copy — select the details and copy them manually.'),
          'warn',
        ),
    );
  };

  return (
    <>
      <div class="detail-head">
        <div>
          <div class="osf-eyebrow kicker">{tr('diagnostics', 'Diagnostics')}</div>
          <h2>{tr('systemHealth', 'System health')}</h2>
        </div>
      </div>

      <div class="detail-body detail-body--health">
        <Summary counts={counts} overall={overall} tr={tr} />

        <div class="health-actions">
          <button
            type="button"
            class="osf-btn osf-btn--sm"
            disabled={reportChecking}
            onClick={beginReport}
          >
            {reportChecking
              ? tr('checkingReporter', 'Checking reporter…')
              : tr('reportBug', 'Report a bug')}
          </button>
          <button
            type="button"
            class="osf-btn osf-btn--sm osf-btn--ghost"
            onClick={() =>
              copyText(serializeReport(health), tr('reportCopied', 'Diagnostic report copied'))
            }
          >
            {tr('copyReport', 'Copy diagnostic report')}
          </button>
          <button
            type="button"
            class="osf-btn osf-btn--sm osf-btn--ghost"
            onClick={() => onShellCommand('osfui.openLogFolder')}
          >
            {tr('openLogFolder', 'Open log folder')}
          </button>
          <a
            class="osf-btn osf-btn--sm osf-btn--ghost"
            href={NEXUS_PAGE_URL}
            target="_blank"
            rel="noreferrer"
          >
            {tr('troubleshooting', 'Troubleshooting')}
          </a>
        </div>

        {reportOpen && reportStatus ? (
          <section class="health-report" aria-label={tr('reportBug', 'Report a bug')}>
            <div class="health-report-head">
              <div>
                <div class="group-label">{tr('privateDiagnostics', 'Private diagnostics')}</div>
                <h3>{tr('reportProblem', 'Tell us what went wrong')}</h3>
              </div>
              <button
                type="button"
                class="osf-btn osf-btn--sm osf-btn--ghost"
                disabled={reportSubmitting}
                onClick={() => setReportOpen(false)}
              >
                {tr('cancel', 'Cancel')}
              </button>
            </div>

            {reportResult?.ok ? (
              <div class="health-report-success" role="status">
                <strong>{tr('reportReceived', 'Report received')}</strong>
                <span>
                  {tr('reportReference', 'Reference: {id}', {
                    id: reportResult.reportId || '(unknown)',
                  })}
                </span>
                {reportResult.issueNumber ? (
                  <button
                    type="button"
                    class="osf-btn osf-btn--sm osf-btn--ghost"
                    onClick={() => onOpenReportIssue(reportResult.issueNumber!)}
                  >
                    {tr('openGitHubIssue', 'Open GitHub issue')}
                  </button>
                ) : null}
              </div>
            ) : (
              <>
                <label class="health-report-field">
                  <span>{tr('reportTitle', 'Short title')}</span>
                  <input
                    class="osf-input"
                    maxLength={120}
                    value={reportTitle}
                    onInput={(e) => setReportTitle(e.currentTarget.value)}
                  />
                </label>
                <label class="health-report-field">
                  <span>{tr('reportDescription', 'What happened?')}</span>
                  <textarea
                    class="osf-input"
                    maxLength={6000}
                    rows={4}
                    value={reportDescription}
                    onInput={(e) => setReportDescription(e.currentTarget.value)}
                  />
                </label>
                <label class="health-report-field">
                  <span>{tr('reportReproduction', 'How can we reproduce it? (optional)')}</span>
                  <textarea
                    class="osf-input"
                    maxLength={4000}
                    rows={3}
                    value={reportReproduction}
                    onInput={(e) => setReportReproduction(e.currentTarget.value)}
                  />
                </label>

                <div class="health-report-disclosure">
                  <strong>{tr('reportIncludes', 'This upload includes:')}</strong>
                  <ul>
                    {reportStatus.logs.map((name) => <li key={name}>{name} (recent tail)</li>)}
                    <li>{tr('healthSnapshotIncluded', 'System Health snapshot and version information')}</li>
                  </ul>
                  <p>
                    {tr(
                      'reportPrivacy',
                      'Known account and install paths are redacted before upload. Logs can still name installed mods. Diagnostics are private and deleted after {days} days; the title, description, and reproduction steps above may become a public GitHub issue after server-side abuse checks.',
                      { days: reportStatus.retentionDays },
                    )}
                  </p>
                </div>

                <label class="health-report-consent">
                  <input
                    type="checkbox"
                    checked={reportConsent}
                    onInput={(e) => setReportConsent(e.currentTarget.checked)}
                  />
                  <span>{tr('reportConsent', 'I reviewed what will be sent and consent to this upload.')}</span>
                </label>

                {reportResult && !reportResult.ok ? (
                  <p class="health-report-error" role="alert">
                    {tr('reportFailureCode', 'Submission failed{code}.', {
                      code: reportResult.code ? ` (${reportResult.code})` : '',
                    })}
                  </p>
                ) : null}

                <button
                  type="button"
                  class="osf-btn osf-btn--sm"
                  disabled={
                    reportSubmitting || !reportConsent || !reportTitle.trim() || !reportDescription.trim()
                  }
                  onClick={submitReport}
                >
                  {reportSubmitting
                    ? tr('submittingReport', 'Submitting…')
                    : tr('submitReport', 'Submit report')}
                </button>
              </>
            )}
          </section>
        ) : null}

        {active.length ? (
          <div class="health-list" id="health-active">
            {activeErrors.map((issue) => (
              <IssueCard
                key={issue.id}
                issue={issue}
                tr={tr}
                defaultOpen={issue.id === focusIssueId}
                onRetryView={onRetryView}
                onShellCommand={onShellCommand}
                onCopyDetails={(text) =>
                  copyText(text, tr('detailsCopied', 'Details copied'))
                }
              />
            ))}

            {activeWarnings.length ? (
              <div class="group-label health-tier-label" id="health-warning-tier">
                {tr.plural(
                  'warningTier',
                  activeWarnings.length,
                  'Warning ({count})',
                  'Warnings ({count})',
                )}
              </div>
            ) : null}

            {activeWarnings.map((issue) => (
              <IssueCard
                key={issue.id}
                issue={issue}
                tr={tr}
                compact
                defaultOpen={issue.id === focusIssueId}
                onRetryView={onRetryView}
                onShellCommand={onShellCommand}
                onCopyDetails={(text) =>
                  copyText(text, tr('detailsCopied', 'Details copied'))
                }
              />
            ))}
          </div>
        ) : null}

        {resolved.length ? (
          <div class="health-history">
            <button
              type="button"
              class="group-label health-history-toggle"
              aria-expanded={historyOpen ? 'true' : 'false'}
              onClick={() => setHistoryOpen(!historyOpen)}
            >
              {tr.plural(
                'resolvedThisSession',
                resolved.length,
                'Resolved this session ({count})',
                'Resolved this session ({count})',
              )}
            </button>
            {historyOpen ? (
              <div class="health-list" id="health-resolved">
                {resolved.map((issue) => (
                  <IssueCard
                    key={issue.id}
                    issue={issue}
                    tr={tr}
                    defaultOpen={issue.id === focusIssueId}
                    onRetryView={onRetryView}
                    onShellCommand={onShellCommand}
                    onCopyDetails={(text) =>
                      copyText(text, tr('detailsCopied', 'Details copied'))
                    }
                  />
                ))}
              </div>
            ) : null}
          </div>
        ) : null}

        <SystemInfoBlock health={health} tr={tr} />
      </div>
    </>
  );
}

function Summary({
  counts,
  overall,
  tr,
}: {
  counts: ReturnType<typeof countIssues>;
  overall: Severity | null;
  tr: Translator;
}) {
  const title = !overall
    ? tr('allNominal', 'All systems nominal')
    : overall === 'error'
      ? tr('actionRequired', 'Action required')
      : tr('warningsDetected', 'Warnings detected');

  const detail = !overall
    ? tr('nothingToReport', 'Nothing needs your attention.')
    : [
        counts.errors
          ? tr.plural('errorCount', counts.errors, '{count} error', '{count} errors')
          : '',
        counts.warnings
          ? tr.plural('warningCount', counts.warnings, '{count} warning', '{count} warnings')
          : '',
      ]
        .filter(Boolean)
        .join(' · ');

  return (
    <div class={`health-summary health-summary--${overall ?? 'ok'}`} id="health-summary">
      <SeverityMark severity={overall} tr={tr} />
      <div class="health-summary-text">
        <div class="health-summary-title">{title}</div>
        <div class="health-summary-detail">{detail}</div>
      </div>
    </div>
  );
}

/**
 * Icon + text severity marker. `aria-hidden` is on the glyph only: the word
 * beside it is the accessible name, so the severity survives both a screen
 * reader and a colour-blind reading.
 */
function SeverityMark({
  severity,
  tr,
  compact,
}: {
  severity: Severity | null;
  tr: Translator;
  /** Row form: the word is read but not painted — the row has no space for it. */
  compact?: boolean;
}) {
  const glyph = severity === 'error' ? '✕' : severity === 'warning' ? '!' : '✓';
  const label =
    severity === 'error'
      ? tr('severityError', 'Error')
      : severity === 'warning'
        ? tr('severityWarning', 'Warning')
        : tr('severityOk', 'Nominal');
  return (
    <span class={`health-mark health-mark--${severity ?? 'ok'}`}>
      <span class="health-mark-glyph" aria-hidden="true">
        {glyph}
      </span>
      {/* Hidden, not dropped: severity must survive a screen reader. Sighted
          readers still get two non-colour signals — the glyph differs per
          severity ("✕" vs "!"), so this is not colour-alone. */}
      <span class={compact ? 'health-mark-label health-mark-label--sr' : 'health-mark-label'}>
        {label}
      </span>
    </span>
  );
}

interface IssueCardProps {
  issue: IssueRecord;
  tr: Translator;
  defaultOpen: boolean;
  /** Render as a one-line row that expands in place (the warning tier). */
  compact?: boolean;
  onRetryView: (viewId: string) => void;
  onShellCommand: (command: string) => void;
  onCopyDetails: (text: string) => void;
}

function IssueCard({
  issue,
  tr,
  defaultOpen,
  compact,
  onRetryView,
  onShellCommand,
  onCopyDetails,
}: IssueCardProps) {
  const [open, setOpen] = useState(defaultOpen);
  // A compact row carries a second state: whether the row itself is unfolded.
  // A deep link opens both, so arriving at a warning shows it in full.
  const [rowOpen, setRowOpen] = useState(defaultOpen);
  const copy = copyForIssue(issue);
  // Every line of copy goes through the same substitution: the mod-reported
  // fallback names its mod via `{mod}`, and the fixed platform strings simply
  // have nothing to substitute.
  const line = (pair: [string, string]) => tr(pair[0], pair[1], copy.params);
  const severity = severityOf(issue);
  const resolvedCard = isResolved(issue);

  const detailsText = technicalText(issue);

  const runAction = (kind: ActionKind) => {
    switch (kind) {
      case 'retry-view':
        // The argument is the issue's own subject — a view id the runtime
        // already knows. Nothing free-text ever reaches a command.
        if (issue.subject) onRetryView(issue.subject);
        return;
      case 'update-osfui':
        onShellCommand('osfui.openModPage');
        return;
      case 'open-logs':
        onShellCommand('osfui.openLogFolder');
        return;
      case 'copy-details':
        // Show what is about to be copied: if the clipboard refuses, the text
        // is already on screen and selectable.
        setOpen(true);
        onCopyDetails(detailsText);
        return;
    }
  };

  const tag = resolvedCard ? (
    <span class="health-card-tag">{tr('resolved', 'Resolved')}</span>
  ) : (issue.occurrences ?? 1) > 1 ? (
    <span
      class="health-card-tag"
      title={tr('occurrenceHint', 'How many times this happened this session')}
    >
      {tr('timesCount', '{count}×', { count: issue.occurrences ?? 1 })}
    </span>
  ) : null;

  // Everything below the heading. Identical in both forms — a compact row is a
  // smaller door onto the same content, never a reduced version of it.
  const body = (
    <>
      <p class="health-card-impact">{line(copy.impact)}</p>
      <p class="health-card-next">{line(copy.next)}</p>

      <div class="health-card-actions">
        {copy.actions.map((kind) =>
          kind === 'retry-view' && !canRetryView(issue) ? null : (
            <button
              key={kind}
              type="button"
              class="osf-btn osf-btn--sm osf-btn--ghost"
              onClick={() => runAction(kind)}
            >
              {actionLabel(kind, tr)}
            </button>
          ),
        )}
        <button
          type="button"
          class="osf-btn osf-btn--sm osf-btn--ghost health-card-disclose"
          aria-expanded={open ? 'true' : 'false'}
          onClick={() => setOpen(!open)}
        >
          {open
            ? tr('hideTechnical', 'Hide technical details')
            : tr('showTechnical', 'Technical details')}
        </button>
      </div>

      {open ? (
        <pre class="health-card-technical" tabIndex={0}>
          {detailsText}
        </pre>
      ) : null}
    </>
  );

  if (compact) {
    return (
      <article
        class={`health-card health-card--row health-card--${severity}${
          rowOpen ? ' health-card--row-open' : ''
        }${resolvedCard ? ' health-card--resolved' : ''}`}
        data-issue={issue.id}
        data-code={issue.code || ''}
      >
        {/* The whole row is the control: a one-line summary that is its own
            disclosure button, so there is no separate hit target to find. */}
        <button
          type="button"
          class="health-row-head"
          aria-expanded={rowOpen ? 'true' : 'false'}
          onClick={() => setRowOpen(!rowOpen)}
        >
          <SeverityMark severity={resolvedCard ? null : severity} tr={tr} compact />
          <span class="health-row-title">{line(copy.title)}</span>
          {issue.subject ? <span class="health-row-subject">{issue.subject}</span> : null}
          {tag}
          <span class="health-row-chevron" aria-hidden="true">
            {rowOpen ? '▾' : '▸'}
          </span>
        </button>
        {rowOpen ? <div class="health-row-body">{body}</div> : null}
      </article>
    );
  }

  return (
    <article
      class={`health-card health-card--${severity}${resolvedCard ? ' health-card--resolved' : ''}`}
      data-issue={issue.id}
      data-code={issue.code || ''}
    >
      <header class="health-card-head">
        <SeverityMark severity={resolvedCard ? null : severity} tr={tr} />
        <div class="health-card-heading">
          <h3 class="health-card-title">{line(copy.title)}</h3>
          {issue.subject ? <div class="health-card-subject">{issue.subject}</div> : null}
        </div>
        {tag}
      </header>

      {body}
    </article>
  );
}

function actionLabel(kind: ActionKind, tr: Translator): string {
  switch (kind) {
    case 'retry-view':
      return tr('retryView', 'Retry view');
    case 'update-osfui':
      return tr('updateOsfui', 'Update OSF UI');
    case 'open-logs':
      return tr('openLogFolder', 'Open log folder');
    case 'copy-details':
      return tr('copyDetails', 'Copy details');
  }
}

/**
 * The card's collapsed disclosure: the stable code, raw native error text,
 * occurrence count and session timing. Deliberately not translated — this is
 * what gets pasted into a bug report, and it has to read the same everywhere.
 */
export function technicalText(issue: IssueRecord): string {
  const lines = [
    `code: ${issue.code || '(none)'}`,
    `id: ${issue.id}`,
    `severity: ${severityOf(issue)}`,
    `status: ${issue.status || 'active'}`,
  ];
  if (issue.subject) lines.push(`subject: ${issue.subject}`);
  if (issue.source) lines.push(`source: ${issue.source}`);
  lines.push(`occurrences: ${issue.occurrences ?? 1}`);
  lines.push(
    `session: first ${issue.firstAt ?? 0}s, last ${issue.lastAt ?? 0}s` +
      (isResolved(issue) ? `, resolved ${issue.resolvedAt ?? 0}s` : ''),
  );
  const context = issue.context || {};
  for (const key of Object.keys(context)) lines.push(`${key}: ${String(context[key])}`);
  return lines.join('\n');
}

function SystemInfoBlock({ health, tr }: { health: HealthModel; tr: Translator }) {
  const system = health.system || {};
  const keys = Object.keys(system);
  return (
    <div class="health-system" id="health-system">
      <div class="group-label health-system-label">{tr('systemInformation', 'System information')}</div>
      {keys.length ? (
        <dl class="health-system-list">
          {keys.map((key) => (
            <div key={key} class="health-system-row">
              <dt>{key}</dt>
              <dd>{String(system[key])}</dd>
            </div>
          ))}
        </dl>
      ) : (
        <p class="detail-quiet">{tr('systemInfoUnavailable', 'Not available yet.')}</p>
      )}
    </div>
  );
}
