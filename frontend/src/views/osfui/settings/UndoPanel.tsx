
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
  // The chip that opens this is hidden at zero anyway; belt to that braces.
  if (!changes.length) return null;

  return (
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
                {/* JSON.stringify, not a friendly formatter: shows the stored
                    value unambiguously ("true" vs "\"true\"", [] vs ""),
                    because that is what the revert writes back. */}
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
