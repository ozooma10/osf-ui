
import type { BindingRow } from '@lib/keybinds/model';

export function matchesQuery(q: string): (b: BindingRow) => boolean {
  return (b) =>
    !q ||
    b.name.toLowerCase().includes(q) ||
    b.keyLabel.toLowerCase().includes(q) ||
    b.label.toLowerCase().includes(q) ||
    b.owner.toLowerCase().includes(q);
}
