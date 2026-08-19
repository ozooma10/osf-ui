
import type { ComponentChildren } from 'preact';
import { cx } from './cx';
import { optAttr } from './optAttr';

export interface RowProps {
  children: ComponentChildren;
  class: string;
  /** `data-key`; "" omits the attribute. The search-jump anchor. */
  dataKey: string;
}

export function Row({ children, class: extra, dataKey }: RowProps) {
  return (
    <div class={cx('row', extra)} {...optAttr('data-key', dataKey)}>
      {children}
    </div>
  );
}
