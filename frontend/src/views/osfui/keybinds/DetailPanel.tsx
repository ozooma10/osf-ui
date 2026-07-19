// DetailPanel.tsx — everything bound to the selected key.
//
// Ports `renderDetail()` (main.legacy.js:311-338) together with the panel
// chrome that used to be static markup (index.html:62-65).
//
// NOTE WHAT IS ABSENT: `query`. Legacy calls renderDetail() only from
// selectKey() and renderAll(), never from the search input handler
// (main.legacy.js:518 paints the board and the list and nothing else), so
// typing in the search box does NOT re-scope this panel. Taking no query prop
// is how that guarantee is enforced here rather than merely observed.

import { holdersOf, keyState } from '@lib/keybinds/conflicts';
import type { BindingRow } from '@lib/keybinds/model';
import type { Translator } from '@lib/i18n';
import { HolderRow, holderInstanceId } from './HolderRow';

export interface DetailPanelProps {
  bindings: readonly BindingRow[];
  selectedKey: string;
  /**
   * False until the first render that had data (legacy's `renderAll`). Before
   * that the panel shows only its static title, because legacy's renderDetail
   * had simply not run yet — the hint paragraph appears with the first payload.
   */
  loaded: boolean;
  tr: Translator;
  capturingId: string | null;
  onRebind: (binding: BindingRow, instanceId: string) => void;
}

export function DetailPanel(props: DetailPanelProps) {
  const { bindings, selectedKey, loaded, tr, capturingId, onRebind } = props;

  const holders = selectedKey ? holdersOf(bindings, selectedKey) : [];
  const state = selectedKey ? keyState(bindings, selectedKey) : { conflict: false, shared: false };

  return (
    <section class="kb-panel kb-detail-panel" aria-live="polite">
      <div class="osf-eyebrow kb-panel-title" id="detail-title">
        {selectedKey ? (
          <>
            <span class="kb-chip kb-chip--lg">{selectedKey}</span>
            {/* The leading space is a real text node in legacy
                (`document.createTextNode(" " + ...)`), not CSS spacing. */}
            {holders.length
              ? ` ${tr.plural('bindingCount', holders.length, '{count} binding', '{count} bindings')}`
              : ` ${tr('unbound', 'unbound')}`}
            {/* Both badges can show at once — keyState reports the flags
                independently, and three holders can genuinely be both. */}
            {state.conflict ? (
              <span class="osf-badge osf-badge--stop">{tr('keyConflict', 'Key conflict')}</span>
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
          // Reachable: a key can be selected and then have its last holder
          // rebound away, and `settings.changed` repaints without clearing the
          // selection.
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
