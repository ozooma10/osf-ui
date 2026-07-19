// UndoPanel.tsx — "changed this visit", with a revert per row.
//
// Ports `openSessionPanel` (settings/main.legacy.js:1490-1534).
//
// THIS IS NOT AN "UNSAVED CHANGES" DIALOG, and the copy goes out of its way to
// say so. Persistence is write-behind and automatic: everything listed here is
// ALREADY saved to disk. What the panel offers is an UNDO facility scoped to
// "since you opened settings this time" — which is precisely why the baseline
// is dropped on every `ui.visibility` open edge. Framing it as unsaved work
// would train users to look for a Save button that does not exist.
//
// The overlay carries `data-nav-modal="1"` via @ui/Overlay: while it is in the
// document, padnav enumerates candidates only inside it and drops an `active`
// element outside it (src/legacy/padnav.js:76, 211-212). That attribute IS the
// focus trap — there is no other mechanism.

import { Overlay } from '@ui/Overlay';
import { titleOf } from '@lib/settings/rail';
import type { SessionChange } from '@lib/settings/modified';
import type { Translator } from '@lib/i18n';

export interface UndoPanelProps {
  changes: SessionChange[];
  tr: Translator;
  onRevert: (change: SessionChange) => void;
  onRevertAll: (changes: SessionChange[]) => void;
  onClose: () => void;
}

export function UndoPanel({ changes, tr, onRevert, onRevertAll, onClose }: UndoPanelProps) {
  // Legacy returned early rather than opening an empty panel
  // (main.legacy.js:1502). The chip that opens it is hidden at zero anyway, so
  // this is the belt to that braces.
  if (!changes.length) return null;

  return (
    {/* Click-outside closes: the handler is on the OVERLAY and tests
        `e.target === e.currentTarget`, so a click that lands on the panel (or
        anything in it) bubbles up without matching and the panel stays put. */}
    <Overlay
      class="session-overlay"
      onClick={(e) => {
        if (e.target === e.currentTarget) onClose();
      }}
    >
      <div class="session-panel osf-card">
        <div class="session-head">
          <div class="osf-eyebrow">
            {tr('changedVisit', 'Changed this visit ({count})', { count: changes.length })}
          </div>
          <button
            type="button"
            class="osf-btn osf-btn--sm osf-btn--danger"
            onClick={() => onRevertAll(changes)}
          >
            {tr('revertAll', 'Revert all')}
          </button>
        </div>
        <p class="session-note">
          {tr(
            'sessionCopy',
            "Everything is already saved. Revert anything you've changed since opening settings.",
          )}
        </p>
        <div class="session-list">
          {changes.map((c) => (
            <div key={`${c.modId} ${c.key}`} class="session-row">
              <div class="session-info">
                <div class="session-key">{`${titleOf(c.mod)} · ${c.key}`}</div>
                {/* JSON.stringify, not a friendly formatter: the point is to
                    show the STORED value unambiguously — "true" vs "\"true\"",
                    [] vs "" — because that is what the revert writes back. */}
                <div class="session-delta">{`${JSON.stringify(c.old)} → ${JSON.stringify(c.now)}`}</div>
              </div>
              <button
                type="button"
                class="osf-btn osf-btn--sm osf-btn--ghost"
                onClick={() => onRevert(c)}
              >
                {tr('revert', 'Revert')}
              </button>
            </div>
          ))}
        </div>
      </div>
    </Overlay>
  );
}
