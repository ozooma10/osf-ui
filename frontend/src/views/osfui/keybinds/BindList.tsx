// The searchable "All bindings" list, plus its panel chrome.

import { holderState } from '@lib/keybinds/conflicts';
import { compareBindings } from '@lib/keybinds/sort';
import type { BindingRow } from '@lib/keybinds/model';
import type { Translator } from '@lib/i18n';
import { HolderRow, holderInstanceId } from './HolderRow';
import { matchesQuery } from './search';

export interface BindListProps {
  bindings: readonly BindingRow[];
  /** Already trimmed + lowercased by the caller, per matchesQuery(). */
  query: string;
  /** The title stays uncounted until data lands. */
  loaded: boolean;
  tr: Translator;
  capturingId: string | null;
  onRebind: (binding: BindingRow, instanceId: string) => void;
  onSelect: (name: string) => void;
}

export function BindList(props: BindListProps) {
  const { bindings, query, loaded, tr, capturingId, onRebind, onSelect } = props;

  // filter already returns a fresh array, so the in-place sort can't disturb
  // the model.
  const rows = bindings.filter(matchesQuery(query)).sort(compareBindings);

  return (
    <section class="kb-panel kb-list-panel">
      <div class="osf-eyebrow kb-panel-title" id="list-title">
        {loaded
          ? tr('allBindingsCount', 'All bindings ({count})', { count: rows.length })
          : tr('allBindings', 'All bindings')}
      </div>
      <div id="bindlist" class="kb-list">
        {!loaded
          ? null
          : rows.length
            ? rows.map((b) => {
                // holderState compares by identity, so `b` must be a row from
                // the array being queried — `filter` copies references, it
                // does not clone rows.
                const state = holderState(bindings, b);
                const stateClass = state.conflict
                  ? 'kb-holder--conflict'
                  : state.shared
                    ? 'kb-holder--shared'
                    : '';
                const instanceId = holderInstanceId('list', b);
                return (
                  <HolderRow
                    key={instanceId}
                    binding={b}
                    tr={tr}
                    instanceId={instanceId}
                    capturingId={capturingId}
                    onRebind={onRebind}
                    list={{ stateClass, onSelect }}
                  />
                );
              })
            : (
              // Two empty states: a filter that matched nothing vs. a registry
              // with no key settings at all.
              <p class="kb-hint">
                {query
                  ? tr('noMatches', 'No bindings match.')
                  : tr('noneRegistered', 'No key bindings registered.')}
              </p>
            )}
      </div>
    </section>
  );
}
