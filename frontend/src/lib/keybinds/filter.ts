// Shared filtering and priority policy for the Keybinds surface's Layer picker.

import type { BindingRow } from '@lib/keybinds/model';
import type { GameplayMode, InputContextState } from '@sdk';

function modeMatches(binding: BindingRow, mode: GameplayMode): boolean {
  return !binding.gameplayModes || binding.gameplayModes.includes(mode);
}

export function matchesBindingFilter(
  binding: BindingRow,
  filter: string,
  input: InputContextState | null,
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
    if (!input?.available) return false;
    if (binding.kind === 'game') {
      return input.contexts.some((context) => context.id === binding.contextNumericId);
    }
    return !!input.mode && modeMatches(binding, input.mode);
  }
  if (filter.startsWith('category:')) return binding.category === filter.slice('category:'.length);
  return true;
}

/**
 * Keep every selected-key holder visible, but bring the rows belonging to the
 * selected Layer to the front. The two buckets preserve their source order so
 * changing Layer never causes unrelated rows to shuffle among themselves.
 */
export function prioritizeBindingsForFilter(
  bindings: readonly BindingRow[],
  filter: string,
  input: InputContextState | null,
): BindingRow[] {
  if (filter === 'all') return [...bindings];
  const prioritized: BindingRow[] = [];
  const remaining: BindingRow[] = [];
  for (const binding of bindings) {
    (matchesBindingFilter(binding, filter, input) ? prioritized : remaining).push(binding);
  }
  return prioritized.concat(remaining);
}
