// Ordering for the "All bindings" list.

import type { BindingRow } from './model';

export function keyOrder(name: string): string {
  const f = /^F(\d+)$/.exec(name);
  const digits = f?.[1];
  return digits !== undefined ? `0${String(parseInt(digits, 10)).padStart(3, '0')}` : `1${name}`;
}

export function compareBindings(a: BindingRow, b: BindingRow): number {
  return keyOrder(a.name).localeCompare(keyOrder(b.name)) || a.owner.localeCompare(b.owner);
}
