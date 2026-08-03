// DOM KeyboardEvent -> OSF UI key name.
//
// Standalone preview only: in game the capture path is native
// (settings.captureKey -> settings.captured) and names the key from its
// physical scan code. This exists so the view works in a plain browser with no
// bridge.
//
// `e.code` is consulted FIRST: it is the physical-key identity (W3C code
// values map 1:1 onto scan codes), so the preview agrees with native on every
// keyboard layout — the whole point of the physical re-anchor. The legacy
// `e.key` table remains as a fallback for environments without `code`.

/** W3C KeyboardEvent.code -> OSF UI key name. Missing => not bindable. */
const CODES: Readonly<Record<string, string>> = {
  Space: 'Space',
  Enter: 'Enter',
  Tab: 'Tab',
  Backspace: 'Backspace',
  CapsLock: 'CapsLock',
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
  // Sided modifiers: `e.code` can tell left from right where `e.key` never
  // could — the preview finally captures them like native does.
  ShiftLeft: 'LShift',
  ShiftRight: 'RShift',
  ControlLeft: 'LCtrl',
  ControlRight: 'RCtrl',
  AltLeft: 'LAlt',
  AltRight: 'RAlt',
  // OEM punctuation by PHYSICAL position — matches kNamedScans in
  // InputRouter.cpp on any layout.
  Backquote: 'Grave',
  Minus: 'Minus',
  Equal: 'Equals',
  BracketLeft: 'LBracket',
  BracketRight: 'RBracket',
  Backslash: 'Backslash',
  Semicolon: 'Semicolon',
  Quote: 'Apostrophe',
  Comma: 'Comma',
  Period: 'Period',
  Slash: 'Slash',
  IntlBackslash: 'IntlBackslash',
  IntlRo: 'IntlRo',
  IntlYen: 'IntlYen',
  NumLock: 'NumLock',
  ScrollLock: 'ScrollLock',
  Pause: 'Pause',
  PrintScreen: 'PrintScreen',
  ContextMenu: 'Apps',
  NumpadEnter: 'NumpadEnter',
  NumpadDivide: 'NumpadDivide',
  NumpadMultiply: 'NumpadMultiply',
  NumpadSubtract: 'NumpadSubtract',
  NumpadAdd: 'NumpadAdd',
  NumpadDecimal: 'NumpadDecimal',
  // Escape is deliberately absent (the capture flow reads it as cancel), as
  // are MetaLeft/MetaRight (capture-reserved: the Start menu owns Win keyups).
};

/** Legacy named-key table for the `e.key` fallback. */
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
  // Keyed by the produced character — only reached when `e.code` is absent,
  // where US-layout assumptions are the best remaining guess.
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

const F_KEY = /^F([1-9]|1[0-9]|2[0-4])$/;

/**
 * Map a keydown event to an OSF UI key name, or "" when the key is not
 * bindable in the preview.
 *
 * Branch order:
 *   1. `e.code` (physical, layout-independent — agrees with native):
 *      KeyX / DigitN strip their prefixes, F1-F24 and NumpadN pass through,
 *      everything else through the CODES table.
 *   2. No `code` (older embedder, synthetic event): the legacy `e.key`
 *      character path — F-keys verbatim, single letter uppercased, single
 *      digit verbatim, then the NAMED table.
 *   3. Miss => "" (= treated as a cancel by the capture flow).
 */
export function domKeyName(e: { key: string; code?: string }): string {
  const code = e.code;
  if (typeof code === 'string' && code) {
    if (/^Key[A-Z]$/.test(code)) return code.slice(3);
    if (/^Digit[0-9]$/.test(code)) return code.slice(5);
    if (/^Numpad[0-9]$/.test(code)) return code;
    if (F_KEY.test(code)) return code;
    // Own-property-guarded: a bare `CODES[code]` would return inherited
    // members for "constructor" etc. Unreachable via the UI Events `code`
    // vocabulary, but cheap to hold (same stance as NAMED below).
    return Object.prototype.hasOwnProperty.call(CODES, code) ? CODES[code] || '' : '';
  }

  const key = e.key;
  if (F_KEY.test(key)) return key;
  if (/^[a-z]$/i.test(key)) return key.toUpperCase();
  if (/^[0-9]$/.test(key)) return key;
  return Object.prototype.hasOwnProperty.call(NAMED, key) ? NAMED[key] || '' : '';
}
