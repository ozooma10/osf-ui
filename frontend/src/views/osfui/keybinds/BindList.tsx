// The searchable "All bindings" list, plus its panel chrome.

import { holderState } from '@lib/keybinds/conflicts';
import { matchesBindingFilter } from '@lib/keybinds/filter';
import { compareBindings } from '@lib/keybinds/sort';
import type { BindingRow } from '@lib/keybinds/model';
import type { Translator } from '@lib/i18n';
import type { EngineInputContextState } from '@sdk';
import { Dropdown } from '@ui/Dropdown';
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
  filter: string;
  onFilter: (filter: string) => void;
  engineInputContext: EngineInputContextState | null;
}

export function BindList(props: BindListProps) {
  const { bindings, query, loaded, tr, capturingId, onRebind, onSelect, filter, onFilter, engineInputContext } = props;

  const rows = bindings
    .filter(matchesQuery(query))
    .filter((row) => matchesBindingFilter(row, filter, engineInputContext))
    .sort(compareBindings);
  const categories = [...new Set(bindings.flatMap((binding) =>
    binding.kind === 'game' && binding.category ? [binding.category] : []))];
  const filterOptions = [
    { value: 'all', label: tr('filterAll', 'All') },
    { value: 'active', label: tr('filterActive', 'Active now') },
    { value: 'gameplay', label: tr('filterGameplay', 'Gameplay') },
    { value: 'ship', label: tr('filterShip', 'Ship') },
    { value: 'vehicle', label: tr('filterVehicle', 'Vehicle') },
    { value: 'menu', label: tr('filterMenu', 'Menu') },
    { value: 'other', label: tr('filterOther', 'Other') },
    { value: 'unbound', label: tr('filterUnbound', 'Unbound') },
    ...categories.map((category) => ({ value: `category:${category}`, label: category })),
  ];

  return (
    <section class="kb-panel kb-list-panel">
      <div class="osf-eyebrow kb-panel-title" id="list-title">
        {loaded
          ? tr('allBindingsCount', 'All bindings ({count})', { count: rows.length })
          : tr('allBindings', 'All bindings')}
      </div>
      <Dropdown
        id="binding-filter"
        class="kb-filter"
        menuClass="kb-filter-menu"
        ariaLabel={tr('filterBindings', 'Filter bindings')}
        value={filter}
        options={filterOptions}
        disabled={false}
        onCommit={onFilter}
      />
      <div id="bindlist" class="kb-list">
        {!loaded
          ? null
          : rows.length
            ? rows.map((b) => {
                const state = holderState(bindings, b);
                const stateClass = state.conflict
                  ? 'kb-holder--conflict'
                  : state.possible
                    ? 'kb-holder--possible'
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
              <p class="kb-hint">
                {query
                  ? tr('noMatches', 'No bindings match.')
                  : tr('noneRegistered', 'No key bindings assigned.')}
              </p>
            )}
      </div>
    </section>
  );
}
