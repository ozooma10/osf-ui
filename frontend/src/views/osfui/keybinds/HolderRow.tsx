// One binding, rendered identically in the detail panel and in the
// all-bindings list.

import type { BindingRow } from '@lib/keybinds/model';
import type { Translator } from '@lib/i18n';

/** List-only decoration. `null` renders the plain detail-panel variant. */
export interface HolderListMode {
  /**
   * "kb-holder--conflict" | "kb-holder--shared" | "", computed by the caller
   * from holderState(). Mutually exclusive: a row that is both shows only the
   * conflict stripe.
   */
  stateClass: string;
  onSelect: (name: string) => void;
}

export interface HolderRowProps {
  binding: BindingRow;
  tr: Translator;
  /**
   * Identity of this rendered instance, not of the binding. The same (mod, key)
   * can be on screen twice — detail panel and list — and only the clicked copy
   * shows "Press a key…"; keying on the binding alone would light up both.
   * Scope + content, not index, so a search that reorders the list cannot move
   * the armed state onto a different row mid-capture.
   */
  instanceId: string;
  /** instanceId of the armed capture, or null when none is. */
  capturingId: string | null;
  onRebind: (binding: BindingRow, instanceId: string) => void;
  list: HolderListMode | null;
}

/** The stable per-instance key described on `instanceId`. */
export function holderInstanceId(scope: string, b: BindingRow): string {
  return `${scope}:${b.kind}:${b.mod || ''}:${b.key}`;
}

export function HolderRow(props: HolderRowProps) {
  const { binding: b, tr, instanceId, capturingId, onRebind, list } = props;
  const listening = capturingId === instanceId;

  // Class order is fixed: base, then kb-holder--list, then the conflict/shared
  // stripe.
  let className = 'kb-holder';
  if (list) {
    className += ' kb-holder--list';
    if (list.stateClass) className += ` ${list.stateClass}`;
  }

  // Game rows are identified by the engine controlmap event; mod rows by
  // "<modId>.<settingKey>".
  const identity = b.kind === 'game' ? `controlmap · ${b.key}` : `${b.mod}.${b.key}`;

  // One spread so the two list-only attributes cannot get out of step: a
  // focusable row with no activation, or an activating row nothing can focus,
  // would each half-break controller support.
  const rowAttrs = list
    ? {
        // padnav contract: the row is click-to-select, so it must be focusable
        // for Enter/A to reach it. Detail-panel rows are not focusable — they
        // have no row-level action.
        tabIndex: 0,
        onClick: (e: MouseEvent) => {
          // A click on (or inside) the Rebind button stays a rebind — without
          // this the row's select also fires and changes the detail panel out
          // from under the capture.
          const target = e.target as Element | null;
          if (target && target.closest && target.closest('button')) return;
          list.onSelect(b.name);
        },
      }
    : {};

  return (
    <div class={className} {...rowAttrs}>
      <div class="kb-holder-text">
        <div class="kb-holder-title">
          <span>{b.label}</span>
          <span
            class={`osf-badge ${b.kind === 'game' ? 'osf-badge--ghost' : 'osf-badge--osf-accent'}`}
          >
            {b.kind === 'game' ? tr('gameBadge', 'GAME') : b.owner}
          </span>
          {/* Context badge is mod-only and suppressed for the implicit
              default — a chip on every row would be noise. */}
          {b.kind === 'mod' && b.contextId !== 'gameplay' ? (
            <span class="osf-badge kb-context">{b.contextLabel}</span>
          ) : null}
        </div>
        <div class="kb-holder-sub">{`${identity} · ${b.contextLabel}`}</div>
      </div>
      <span class="kb-chip">{b.name}</span>
      {b.kind === 'mod' ? (
        <button
          type="button"
          // padnav suspends all navigation while any `.listening` element
          // exists — the next key press belongs to the capture.
          class={`osf-btn osf-btn--sm osf-key${listening ? ' listening' : ''}`}
          onClick={() => onRebind(b, instanceId)}
        >
          {listening ? tr('pressKey', 'Press a key…') : tr('rebind', 'Rebind')}
        </button>
      ) : null}
    </div>
  );
}
