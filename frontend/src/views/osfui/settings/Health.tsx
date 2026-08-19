
import { useEffect, useState } from 'preact/hooks';
import type { Translator } from '@lib/i18n';
import {
  activeIssues,
  canRetryView,
  copyForIssue,
  countIssues,
  isResolved,
  overallSeverity,
  resolvedIssues,
  severityOf,
  type ActionKind,
  type HealthModel,
  type IssueRecord,
  type Severity,
} from '@lib/settings/health';


export interface HealthProps {
  health: HealthModel;
  tr: Translator;
  /** Issue id to expand and scroll to on mount (deep link from a failed card). */
  focusIssueId: string | null;
  /** menu.open on a view id — the only action that takes an argument. */
  onRetryView: (viewId: string) => void;
  onToast: (message: string, kind?: 'warn' | 'danger') => void;
}

export function Health({
  health,
  tr,
  focusIssueId,
  onRetryView,
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

  const resolvedTarget = !!focusIssueId && resolved.some((i) => i.id === focusIssueId);
  useEffect(() => {
    if (resolvedTarget) setHistoryOpen(true);
  }, [resolvedTarget]);

  useEffect(() => {
    if (!focusIssueId) return;
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
          {/* Compatibility catalog address; fallback copy uses the canonical view name. */}
          <div class="osf-eyebrow kicker">{tr('diagnostics', 'Mod Settings')}</div>
          <h2>{tr('systemHealth', 'System Health')}</h2>
        </div>
      </div>

      <div class="detail-body detail-body--health">
        <Summary counts={counts} overall={overall} tr={tr} />

        {active.length ? (
          <div class="health-list" id="health-active">
            {activeErrors.map((issue) => (
              <IssueCard
                key={issue.id}
                issue={issue}
                tr={tr}
                defaultOpen={issue.id === focusIssueId}
                onRetryView={onRetryView}
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
    ? tr('allNominal', 'No active issues')
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
  onCopyDetails: (text: string) => void;
}

function IssueCard({
  issue,
  tr,
  defaultOpen,
  compact,
  onRetryView,
  onCopyDetails,
}: IssueCardProps) {
  const [open, setOpen] = useState(defaultOpen);
  const [rowOpen, setRowOpen] = useState(defaultOpen);
  const copy = copyForIssue(issue);
  const line = (pair: [string, string]) => tr(pair[0], pair[1], copy.params);
  const severity = severityOf(issue);
  const resolvedCard = isResolved(issue);

  const detailsText = technicalText(issue);

  const runAction = (kind: ActionKind) => {
    switch (kind) {
      case 'retry-view':
        if (issue.subject) onRetryView(issue.subject);
        return;
      case 'copy-details':
        setOpen(true);
        onCopyDetails(detailsText);
        return;
    }
  };

  const tag = resolvedCard ? (
    <span class="health-card-tag">{tr('resolved', 'Resolved')}</span>
  ) : issue.occurrences > 1 ? (
    <span
      class="health-card-tag"
      title={tr('occurrenceHint', 'How many times this happened this session')}
    >
      {tr('timesCount', '{count}×', { count: issue.occurrences })}
    </span>
  ) : null;

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
        data-code={issue.code}
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
      data-code={issue.code}
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
    case 'copy-details':
      return tr('copyDetails', 'Copy details');
  }
}

export function technicalText(issue: IssueRecord): string {
  const lines = [
    `code: ${issue.code || '(none)'}`,
    `id: ${issue.id}`,
    `severity: ${severityOf(issue)}`,
    `status: ${issue.status}`,
  ];
  if (issue.subject) lines.push(`subject: ${issue.subject}`);
  if (issue.source) lines.push(`source: ${issue.source}`);
  lines.push(`occurrences: ${issue.occurrences}`);
  lines.push(
    `session: first ${issue.firstAt}s, last ${issue.lastAt}s` +
      (isResolved(issue) ? `, resolved ${issue.resolvedAt ?? 0}s` : ''),
  );
  const context = issue.context;
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
