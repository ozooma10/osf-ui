// System Health — the pane behind the pinned rail destination.
//
// The promise this pane makes is that everything on it is true right now and
// worth a player's attention. It is not a log viewer: nothing lands here that
// a subsystem did not deliberately raise, and nothing stays here once that
// subsystem withdraws it. That is why there is no dismiss button — a card you
// could dismiss would be a card that could lie.
//
// Severity is carried three ways (word, colour, icon), never by colour alone.
// Everything a native payload supplies is rendered as a text child; the only
// prose in the pane comes from the code->copy table in @lib/settings/diagnostics,
// so it is localizable and cannot be authored by a mod. Raw native text appears
// only under a card's collapsed technical disclosure, where it reads as
// developer detail rather than as UI chrome.

import { useState } from 'preact/hooks';
import type { Translator } from '@lib/i18n';
import {
  activeIssues,
  canRetryView,
  copyForCode,
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

/** Where "Troubleshooting" and "Update OSF UI" send the player. */
const NEXUS_PAGE_URL = 'https://www.nexusmods.com/starfield/mods/17711';

export interface HealthProps {
  health: HealthModel;
  tr: Translator;
  /** Issue id to expand and scroll to on mount (deep link from a failed card). */
  focusIssueId: string | null;
  /** menu.open on a view id — the only action that takes an argument. */
  onRetryView: (viewId: string) => void;
  /** Fire a payload-free shell command (osfui.openLogFolder, osfui.openModPage). */
  onShellCommand: (command: string) => void;
  onToast: (message: string, kind?: 'warn' | 'danger') => void;
}

export function Health({
  health,
  tr,
  focusIssueId,
  onRetryView,
  onShellCommand,
  onToast,
}: HealthProps) {
  const active = activeIssues(health);
  const resolved = resolvedIssues(health);
  const counts = countIssues(health.issues);
  const overall = overallSeverity(counts);
  const [historyOpen, setHistoryOpen] = useState(false);

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

        {active.length ? (
          <div class="health-list" id="health-active">
            {active.map((issue) => (
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
function SeverityMark({ severity, tr }: { severity: Severity | null; tr: Translator }) {
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
      <span class="health-mark-label">{label}</span>
    </span>
  );
}

interface IssueCardProps {
  issue: IssueRecord;
  tr: Translator;
  defaultOpen: boolean;
  onRetryView: (viewId: string) => void;
  onShellCommand: (command: string) => void;
  onCopyDetails: (text: string) => void;
}

function IssueCard({
  issue,
  tr,
  defaultOpen,
  onRetryView,
  onShellCommand,
  onCopyDetails,
}: IssueCardProps) {
  const [open, setOpen] = useState(defaultOpen);
  const copy = copyForCode(issue.code);
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

  return (
    <article
      class={`health-card health-card--${severity}${resolvedCard ? ' health-card--resolved' : ''}`}
      data-issue={issue.id}
      data-code={issue.code || ''}
    >
      <header class="health-card-head">
        <SeverityMark severity={resolvedCard ? null : severity} tr={tr} />
        <div class="health-card-heading">
          <h3 class="health-card-title">{tr(copy.title[0], copy.title[1])}</h3>
          {issue.subject ? <div class="health-card-subject">{issue.subject}</div> : null}
        </div>
        {resolvedCard ? (
          <span class="health-card-tag">{tr('resolved', 'Resolved')}</span>
        ) : (issue.occurrences ?? 1) > 1 ? (
          <span
            class="health-card-tag"
            title={tr('occurrenceHint', 'How many times this happened this session')}
          >
            {tr('timesCount', '{count}×', { count: issue.occurrences ?? 1 })}
          </span>
        ) : null}
      </header>

      <p class="health-card-impact">{tr(copy.impact[0], copy.impact[1])}</p>
      <p class="health-card-next">{tr(copy.next[0], copy.next[1])}</p>

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
