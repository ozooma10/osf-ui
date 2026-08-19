// Shared filtering and priority policy for the Keybindings view's Layer picker.

import type { BindingRow } from '@lib/keybinds/model';
import type { EngineInputContextState, GameplayMode } from '@sdk';

function modeMatches(binding: BindingRow, mode: GameplayMode): boolean {
  return !binding.gameplayModes || binding.gameplayModes.includes(mode);
}

export function matchesBindingFilter(
  binding: BindingRow,
  filter: string,
  engineInputContext: EngineInputContextState | null,
): boolean {
  if (filter === 'all') return true;
  if (filter === 'unbound') return binding.unbound === true;
  if (filter === 'menu') return binding.kind === 'game' && binding.classification === 'menu';
  if (filter === 'other') return binding.kind === 'game' && binding.classification === 'unknown';
  if (filter === 'gameplay') {
    return binding.kind === 'mod'
      || binding.classification === 'core'
      || binding.classification === 'special';
  }
  if (filter === 'ship' || filter === 'vehicle') return modeMatches(binding, filter);
  if (filter === 'active') {
    if (!engineInputContext?.available) return false;
    if (binding.kind === 'game') {
      return engineInputContext.contexts.some((context) => context.id === binding.engineInputContextId);
    }
    return !!engineInputContext.mode && modeMatches(binding, engineInputContext.mode);
  }
  if (filter.startsWith('category:')) {
    return binding.kind === 'game' && binding.category === filter.slice('category:'.length);
  }
  return true;
}

export function prioritizeBindingsForFilter(
  bindings: readonly BindingRow[],
  filter: string,
  engineInputContext: EngineInputContextState | null,
): BindingRow[] {
  if (filter === 'all') return [...bindings];
  const prioritized: BindingRow[] = [];
  const remaining: BindingRow[] = [];
  for (const binding of bindings) {
    (matchesBindingFilter(binding, filter, engineInputContext) ? prioritized : remaining).push(binding);
  }
  return prioritized.concat(remaining);
}
