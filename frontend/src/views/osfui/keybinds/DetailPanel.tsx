// Everything bound to the selected key.
//
// Takes no `query` prop: typing in the search box must not re-scope this panel,
// and the missing prop is what enforces that. Layer does reach the panel, but
// only to prioritize its matches; holders from other layers remain visible.

import { holdersOf, keyState } from '@lib/keybinds/conflicts';
import { prioritizeBindingsForFilter } from '@lib/keybinds/filter';
import type { KeyLabeler } from '@lib/keybinds/labels';
import type { BindingRow } from '@lib/keybinds/model';
import type { Translator } from '@lib/i18n';
import type { EngineInputContextState } from '@sdk';
import { HolderRow, holderInstanceId } from './HolderRow';

export interface DetailPanelProps {
  bindings: readonly BindingRow[];
  selectedKey: string;
  /** Current Layer picker value; matching holders render first, never alone. */
  filter: string;
  engineInputContext: EngineInputContextState | null;
  /** False until the first render that had data; until then only the title shows. */
  loaded: boolean;
  tr: Translator;
  capturingId: string | null;
  /** Localized keycap lookup; undefined per name = show the name itself. */
  labeler?: KeyLabeler;
  onRebind: (binding: BindingRow, instanceId: string) => void;
}

export function DetailPanel(props: DetailPanelProps) {
  const { bindings, selectedKey, filter, engineInputContext, loaded, tr, capturingId, onRebind } = props;
  const chip = selectedKey ? (props.labeler?.(selectedKey) ?? selectedKey) : '';

  const holders = selectedKey
    ? prioritizeBindingsForFilter(holdersOf(bindings, selectedKey), filter, engineInputContext)
    : [];
  const state = selectedKey ? keyState(bindings, selectedKey) : { conflict: false, possible: false, shared: false };

  return (
    <section class="kb-panel kb-detail-panel" aria-live="polite">
      <div class="osf-eyebrow kb-panel-title" id="detail-title">
        {selectedKey ? (
          <>
            <span class="kb-chip kb-chip--lg">{chip}</span>
            {/* The leading space is a text node, not CSS spacing. */}
            {holders.length
              ? ` ${tr.plural('bindingCount', holders.length, '{count} binding', '{count} bindings')}`
              : ` ${tr('unbound', 'unbound')}`}
            {/* Both badges can show at once; keyState reports the flags independently. */}
            {state.conflict ? (
              <span class="osf-badge osf-badge--stop">{tr('keyConflict', 'Key conflict')}</span>
            ) : null}
            {state.possible ? (
              <span class="osf-badge kb-possible-badge">{tr('possibleConflict', 'Possible conflict')}</span>
            ) : null}
            {state.shared ? (
              <span class="osf-badge kb-shared-badge">
                {tr('sharedAcrossContexts', 'Shared across contexts')}
              </span>
            ) : null}
          </>
        ) : (
          tr('selectKey', 'Select a key')
        )}
      </div>
      <div id="detail" class="kb-detail">
        {!loaded ? null : !selectedKey ? (
          <p class="kb-hint">
            {tr('selectKeyHint', 'Click any key on the board to see what holds it.')}
          </p>
        ) : !holders.length ? (
          // Reachable: a selected key can lose its last holder to a rebind, and
          // `settings.changed` repaints without clearing the selection.
          <p class="kb-hint">{tr('nothingBound', 'Nothing is bound here.')}</p>
        ) : (
          holders.map((b) => {
            const instanceId = holderInstanceId('detail', b);
            return (
              <HolderRow
                key={instanceId}
                binding={b}
                tr={tr}
                instanceId={instanceId}
                capturingId={capturingId}
                onRebind={onRebind}
                list={null}
              />
            );
          })
        )}
      </div>
    </section>
  );
}
