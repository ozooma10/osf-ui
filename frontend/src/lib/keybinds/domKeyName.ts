// DOM KeyboardEvent.key -> OSF UI key name.
//
// Standalone preview only: in game the capture path is native
// (settings.captureKey -> settings.captured) and names the key from its
// virtual-key code. This exists so the view works in a plain browser with no
// bridge, and is partial by design — it covers the keys the on-screen board
// draws and nothing else.

/** Named-key table. Anything not listed maps to "" (= not bindable). */
const NAMED: Readonly<Record<string, string>> = {
  ' ': 'Space',
  Enter: 'Enter',
  Tab: 'Tab',
  Backspace: 'Backspace',
  Insert: 'Insert',
  Delete: 'Delete',
  Home: 'Home',
  End: 'End',
  PageUp: 'PageUp',
  PageDown: 'PageDown',
  ArrowUp: 'Up',
  ArrowDown: 'Down',
  ArrowLeft: 'Left',
  ArrowRight: 'Right',
  '`': 'Grave',
  // OEM punctuation, matching InputRouter.cpp KeyName. Keyed by the produced
  // character, so this agrees with native on a US layout and diverges on others
  // exactly as `Grave` already does.
  '-': 'Minus',
  '=': 'Equals',
  '[': 'LBracket',
  ']': 'RBracket',
  '\\': 'Backslash',
  ';': 'Semicolon',
  "'": 'Apostrophe',
  ',': 'Comma',
  '.': 'Period',
  '/': 'Slash',
};

/**
 * Map a keydown event to an OSF UI key name, or "" when the key is not
 * bindable in the preview.
 *
 * Branch order matters:
 *   1. F1-F24 pass through verbatim.
 *   2. A single letter, either case, uppercases. Contrast canonicalName(),
 *      which only uppercases lowercase input.
 *   3. A single digit passes through.
 *   4. The named table; miss => "".
 *
 * Only `e.key` is consulted, never `e.code`, so a non-US layout reports the
 * produced character rather than the physical key. That disagrees with native,
 * which is VK-based; accepted, since this path never runs in game.
 *
 * The modifier keys the board draws (LShift/LCtrl/LAlt and their right twins)
 * are absent from every branch, so pressing Shift in the preview yields "" and
 * finishCapture treats it as a cancel. DOM cannot tell the two sides apart from
 * `e.key` alone.
 */
export function domKeyName(e: { key: string }): string {
  const key = e.key;
  if (/^F([1-9]|1[0-9]|2[0-4])$/.test(key)) return key;
  if (/^[a-z]$/i.test(key)) return key.toUpperCase();
  if (/^[0-9]$/.test(key)) return key;
  // Own-property-guarded: a bare `NAMED[key]` would return the inherited
  // function for "constructor", "toString", etc. and violate the `string`
  // return type. Unreachable via KeyboardEvent.key's fixed vocabulary, but
  // cheap to hold.
  return Object.prototype.hasOwnProperty.call(NAMED, key) ? NAMED[key] || '' : '';
}
