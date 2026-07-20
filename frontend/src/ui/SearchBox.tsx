// SearchBox.tsx — the kit's search field.
//
// Ported from keybinds/index.html:36-41, which the settings view repeats almost
// verbatim. The markup is fixed by osfui.css (`.osf-search` positions the icon
// and the trailing <kbd> around the input), so nothing here is configurable
// except the strings and the input's own extra class.
//
// The inline SVG is copied character-for-character so the component has no
// external asset dependency and retains the exact icon that ships today.

import type { Ref } from 'preact';

export interface SearchBoxProps {
  /** DOM id — the <label for> target, so it must be unique in the page. */
  id: string;
  value: string;
  onInput: (value: string) => void;
  placeholder: string;
  /**
   * Accessible name. Legacy pointed BOTH `placeholder` and `aria-label` at the
   * same catalog address (data-i18n-placeholder / data-i18n-aria-label, both
   * `chrome.keybinds.searchPlaceholder`), so they are always the same string —
   * but they stay separate props because the settings view may not follow suit.
   */
  ariaLabel: string;
  /** Printed in the trailing <kbd> chip, e.g. "Ctrl F". */
  kbd: string;
  /** The `aria-keyshortcuts` token for the same shortcut, e.g. "Control+F". */
  keyshortcuts: string;
  /** Extra class on the <input>, appended after the kit's `osf-input`. */
  inputClass: string;
  /** Needed by the Ctrl+F handler, which focuses AND selects. */
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
