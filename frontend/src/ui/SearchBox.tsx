
import type { Ref } from 'preact';

export interface SearchBoxProps {
  /** DOM id — the <label for> target, so it must be unique in the page. */
  id: string;
  value: string;
  onInput: (value: string) => void;
  placeholder: string;
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
