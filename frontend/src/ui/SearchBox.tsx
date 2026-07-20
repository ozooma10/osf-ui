// SearchBox.tsx — the kit's search field.
//
// The markup is fixed by osfui.css (`.osf-search` positions the icon and the
// trailing <kbd> around the input), so nothing is configurable except the
// strings and the input's extra class. The icon is inline SVG so the component
// has no external asset dependency.

import type { Ref } from 'preact';

export interface SearchBoxProps {
  /** DOM id — the <label for> target, so it must be unique in the page. */
  id: string;
  value: string;
  onInput: (value: string) => void;
  placeholder: string;
  /**
   * Accessible name. Callers today point this and `placeholder` at the same
   * catalog address, but they stay separate props so a view can diverge.
   */
  ariaLabel: string;
  /** Printed in the trailing <kbd> chip, e.g. "Ctrl F". */
  kbd: string;
  /** The `aria-keyshortcuts` token for the same shortcut, e.g. "Control+F". */
  keyshortcuts: string;
  /** Extra class on the <input>, appended after the kit's `osf-input`. */
  inputClass: string;
  /** Needed by the Ctrl+F handler, which focuses and selects. */
  inputRef: Ref<HTMLInputElement>;
}

export function SearchBox(props: SearchBoxProps) {
  return (
    <label class="osf-search" for={props.id}>
      <svg viewBox="0 0 24 24" aria-hidden="true">
        <circle cx="11" cy="11" r="6.5" />
        <path d="m16 16 4 4" />
      </svg>
      <input
        ref={props.inputRef}
        id={props.id}
        class={`osf-input ${props.inputClass}`}
        type="text"
        value={props.value}
        placeholder={props.placeholder}
        aria-label={props.ariaLabel}
        aria-keyshortcuts={props.keyshortcuts}
        autocomplete="off"
        spellcheck={false}
        onInput={(e) => props.onInput((e.target as HTMLInputElement).value)}
      />
      <kbd>{props.kbd}</kbd>
    </label>
  );
}
