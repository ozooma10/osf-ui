
import type { BindingRow } from '@lib/keybinds/model';
import type { Translator } from '@lib/i18n';

/** List-only decoration. `null` renders the plain detail-panel variant. */
export interface HolderListMode {
  stateClass: string;
  onSelect: (name: string) => void;
}

export interface HolderRowProps {
  binding: BindingRow;
  tr: Translator;
  instanceId: string;
  /** instanceId of the armed capture, or null when none is. */
  capturingId: string | null;
  onRebind: (binding: BindingRow, instanceId: string) => void;
  list: HolderListMode | null;
}

/** The stable per-instance key described on `instanceId`. */
export function holderInstanceId(scope: string, b: BindingRow): string {
  const ownerId = b.kind === 'mod' ? b.mod : b.engineInputContextName;
  return `${scope}:${b.rowId || `${b.kind}:${ownerId}:${b.key}:${b.name}`}`;
}

export function HolderRow(props: HolderRowProps) {
  const { binding: b, tr, instanceId, capturingId, onRebind, list } = props;
  const listening = capturingId === instanceId;

  let className = 'kb-holder';
  if (list) {
    className += ' kb-holder--list';
    if (list.stateClass) className += ` ${list.stateClass}`;
  }

  const identity = b.kind === 'game'
    ? `controlmap · ${b.engineInputContextName} · ${b.key}${b.slot ? ` · ${b.slot}` : ''}`
    : `${b.mod}.${b.key}`;
  const groupingLabel = b.kind === 'game'
    ? b.category || b.engineInputContextLabel
    : b.hotkeyContextLabel;

  const rowAttrs = list
    ? {
        tabIndex: 0,
        onClick: (e: MouseEvent) => {
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
          {b.kind === 'mod' && b.hotkeyContextId !== 'gameplay' ? (
            <span class="osf-badge kb-context">{b.hotkeyContextLabel}</span>
          ) : null}
          {b.kind === 'game' && b.classification ? (
            <span class={`osf-badge kb-classification kb-classification--${b.classification}`}>
              {b.classification.toUpperCase()}
            </span>
          ) : null}
        </div>
        <div class="kb-holder-sub">{`${identity} · ${groupingLabel}`}</div>
      </div>
      {/* The localized keycap (falls back to the canonical name when the OSF UI runtime
          published no labels map). Identity stays b.name everywhere else. */}
      <span class="kb-chip">{b.keyLabel}</span>
      {b.kind === 'mod' ? (
        <button
          type="button"
          class={`osf-btn osf-btn--sm osf-key${listening ? ' listening' : ''}`}
          onClick={() => onRebind(b, instanceId)}
        >
          {listening ? tr('pressKey', 'Press a key…') : tr('rebind', 'Rebind')}
        </button>
      ) : null}
    </div>
  );
}
